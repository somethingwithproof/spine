/*
 ex: set tabstop=4 shiftwidth=4 autoindent:
 +-------------------------------------------------------------------------+
 | Copyright (C) 2004-2026 The Cacti Group                                 |
 |                                                                         |
 | This program is free software; you can redistribute it and/or           |
 | modify it under the terms of the GNU Lesser General Public              |
 | License as published by the Free Software Foundation; either            |
 | version 2.1 of the License, or (at your option) any later version.     |
 |                                                                         |
 | This program is distributed in the hope that it will be useful,         |
 | but WITHOUT ANY WARRANTY; without even the implied warranty of          |
 | MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           |
 | GNU Lesser General Public License for more details.                     |
 |                                                                         |
 | You should have received a copy of the GNU Lesser General Public        |
 | License along with this library; if not, write to the Free Software     |
 | Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA           |
 | 02110-1301, USA                                                         |
 |                                                                         |
 +-------------------------------------------------------------------------+
 | spine: a backend data gatherer for cacti                                |
 +-------------------------------------------------------------------------+
 | Integration tests for poller pipeline: host fetch, result validation,   |
 | output_regex, multipart output detection                                |
 +-------------------------------------------------------------------------+
*/

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <mysql.h>

#include "integration_helpers.h"

#include "common.h"
#include "spine.h"

/* spine globals */
config_t set;
php_t *php_processes = NULL;
char start_datetime[20];
char config_paths[CONFIG_PATHS][BUFSIZE];
pool_t *db_pool_local  = NULL;
pool_t *db_pool_remote = NULL;
sem_t available_threads;
sem_t available_scripts;
double start_time;

static int group_setup(void **state) {
	MYSQL *mysql;

	memset(&set, 0, sizeof(set));
	set.log_level = POLLER_VERBOSITY_NONE;
	set.log_destination = LOGDEST_STDOUT;
	set.poller_id = 1;
	set.start_host_id = -1;
	set.end_host_id = -1;

	init_mutexes();

	mysql = test_db_connect();
	if (mysql == NULL) {
		fprintf(stderr, "SKIP: cannot connect to test database\n");
		return -1;
	}

	if (test_create_schema(mysql) != 0) {
		test_db_disconnect(mysql);
		return -1;
	}

	test_cleanup_tables(mysql);

	if (test_insert_fixtures(mysql) != 0) {
		test_db_disconnect(mysql);
		return -1;
	}

	*state = mysql;
	return 0;
}

static int group_teardown(void **state) {
	MYSQL *mysql = (MYSQL *)*state;

	test_drop_tables(mysql);
	test_db_disconnect(mysql);
	*state = NULL;
	return 0;
}

/* ----------------------------------------------------------------
 * Test: fetch host list from database
 * ---------------------------------------------------------------- */
static void test_fetch_host_list(void **state) {
	MYSQL *mysql = (MYSQL *)*state;
	MYSQL_RES *result;
	MYSQL_ROW row;
	int num_rows;

	result = db_query(mysql, LOCAL,
		"SELECT id, hostname FROM host WHERE poller_id = 1");
	assert_non_null(result);

	num_rows = (int)mysql_num_rows(result);
	assert_true(num_rows >= 1);

	row = mysql_fetch_row(result);
	assert_non_null(row);
	assert_non_null(row[0]); /* id */
	assert_non_null(row[1]); /* hostname */

	db_free_result(result);
}

/* ----------------------------------------------------------------
 * Test: fetch poller items for a specific host
 * ---------------------------------------------------------------- */
static void test_fetch_poller_items(void **state) {
	MYSQL *mysql = (MYSQL *)*state;
	MYSQL_RES *result;
	MYSQL_ROW row;
	int num_rows;

	result = db_query(mysql, LOCAL,
		"SELECT local_data_id, action, hostname, arg1 "
		"FROM poller_item WHERE host_id = 1 AND poller_id = 1");
	assert_non_null(result);

	num_rows = (int)mysql_num_rows(result);
	assert_true(num_rows >= 1);

	row = mysql_fetch_row(result);
	assert_non_null(row);
	assert_string_equal(row[2], "127.0.0.1");

	db_free_result(result);
}

/* ----------------------------------------------------------------
 * Test: validate_result accepts valid numeric string
 * ---------------------------------------------------------------- */
static void test_validate_result_numeric(void **state) {
	char buf[64];

	(void)state;

	strcpy(buf, "12345");
	assert_int_equal(validate_result(buf), TRUE);

	strcpy(buf, "3.14159");
	assert_int_equal(validate_result(buf), TRUE);

	strcpy(buf, "-42");
	assert_int_equal(validate_result(buf), TRUE);
}

/* ----------------------------------------------------------------
 * Test: validate_result accepts multipart output (name:value)
 * ---------------------------------------------------------------- */
static void test_validate_result_multipart(void **state) {
	char buf[128];

	(void)state;

	strcpy(buf, "traffic_in:1000");
	assert_int_equal(validate_result(buf), TRUE);

	strcpy(buf, "in:100 out:200");
	assert_int_equal(validate_result(buf), TRUE);
}

/* ----------------------------------------------------------------
 * Test: validate_result rejects garbage data
 * ---------------------------------------------------------------- */
static void test_validate_result_bad_data(void **state) {
	char buf[128];

	(void)state;

	strcpy(buf, "ERROR: no such OID");
	assert_int_equal(validate_result(buf), FALSE);

	strcpy(buf, "");
	assert_int_equal(validate_result(buf), FALSE);
}

/* ----------------------------------------------------------------
 * Test: validate_result handles undefined "U" marker
 * ---------------------------------------------------------------- */
static void test_validate_result_undefined(void **state) {
	char buf[8];

	(void)state;

	SET_UNDEFINED(buf);
	assert_true(IS_UNDEFINED(buf));

	/* "U" alone is not numeric and not multipart; should fail */
	assert_int_equal(validate_result(buf), FALSE);
}

/* ----------------------------------------------------------------
 * Test: validate_result handles NULL
 * ---------------------------------------------------------------- */
static void test_validate_result_null(void **state) {
	(void)state;

	assert_int_equal(validate_result(NULL), FALSE);
}

/* ----------------------------------------------------------------
 * Test: is_multipart_output with delimiters
 * ---------------------------------------------------------------- */
static void test_is_multipart_output(void **state) {
	char buf[128];

	(void)state;

	strcpy(buf, "traffic_in:1000");
	assert_int_equal(is_multipart_output(buf), TRUE);

	strcpy(buf, "key!value");
	assert_int_equal(is_multipart_output(buf), TRUE);

	/* no delimiters at all */
	strcpy(buf, "just_a_string");
	assert_int_equal(is_multipart_output(buf), FALSE);
}

/* ----------------------------------------------------------------
 * Test: is_numeric on various inputs
 * ---------------------------------------------------------------- */
static void test_is_numeric_various(void **state) {
	(void)state;

	assert_int_equal(is_numeric("0"), TRUE);
	assert_int_equal(is_numeric("99999"), TRUE);
	assert_int_equal(is_numeric("-1"), TRUE);
	assert_int_equal(is_numeric("3.14"), TRUE);
	assert_int_equal(is_numeric("abc"), FALSE);
	assert_int_equal(is_numeric("12abc"), FALSE);
}

/* ----------------------------------------------------------------
 * Test: hex string conversion via is_hexadecimal
 * ---------------------------------------------------------------- */
static void test_is_hexadecimal(void **state) {
	(void)state;

	/* is_hexadecimal requires delimiter (space/colon/dash) and length >= 3 */
	assert_int_equal(is_hexadecimal("DE AD BE EF", 0), TRUE);
	assert_int_equal(is_hexadecimal("DE:AD:BE:EF", 0), TRUE);
	assert_int_equal(is_hexadecimal("DEADBEEF", 0), FALSE);
	assert_int_equal(is_hexadecimal("ZZ ZZ", 0), FALSE);
}

/* ----------------------------------------------------------------
 * Test: regex_replace extracts numeric from prefixed value
 * ---------------------------------------------------------------- */
static void test_regex_replace_numeric(void **state) {
	const char *result;

	(void)state;

	/* REGEX_NUMBER matches [-+]*[0-9]*[.][0-9]+ (requires decimal point) */
	result = regex_replace(REGEX_NUMBER, "Gauge32: 3.14 units");
	assert_non_null(result);
	assert_true(strlen(result) > 0);
	/* result is a static thread-local buffer, do not free */
}

/* ----------------------------------------------------------------
 * main
 * ---------------------------------------------------------------- */
int main(void) {
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_fetch_host_list),
		cmocka_unit_test(test_fetch_poller_items),
		cmocka_unit_test(test_validate_result_numeric),
		cmocka_unit_test(test_validate_result_multipart),
		cmocka_unit_test(test_validate_result_bad_data),
		cmocka_unit_test(test_validate_result_undefined),
		cmocka_unit_test(test_validate_result_null),
		cmocka_unit_test(test_is_multipart_output),
		cmocka_unit_test(test_is_numeric_various),
		cmocka_unit_test(test_is_hexadecimal),
		cmocka_unit_test(test_regex_replace_numeric),
	};

	return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
