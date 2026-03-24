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
 | Integration tests for database connectivity functions                   |
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

/* spine globals required by linked object files */
#include "common.h"
#include "spine.h"

config_t set;
php_t *php_processes = NULL;
char start_datetime[20];
char config_paths[CONFIG_PATHS][BUFSIZE];
pool_t *db_pool_local  = NULL;
pool_t *db_pool_remote = NULL;

/* semaphores referenced by locks.c */
sem_t available_threads;
sem_t available_scripts;

/* start_time referenced by util.c */
double start_time;

/*
 * Shared state for the test group.
 * We store the MYSQL handle in cmocka's state pointer.
 */

static int group_setup(void **state) {
	MYSQL *mysql;

	memset(&set, 0, sizeof(set));
	set.log_level = POLLER_VERBOSITY_NONE;
	set.log_destination = LOGDEST_STDOUT;
	set.poller_id = 1;

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
 * Test: connect with valid credentials succeeds
 * ---------------------------------------------------------------- */
static void test_connect_valid(void **state) {
	MYSQL *mysql;

	(void)state;

	mysql = test_db_connect();
	assert_non_null(mysql);
	assert_true(mysql_ping(mysql) == 0);
	test_db_disconnect(mysql);
}

/* ----------------------------------------------------------------
 * Test: connection failure with bad host returns NULL
 * ---------------------------------------------------------------- */
static void test_connect_bad_host(void **state) {
	MYSQL *mysql;
	int timeout = 1;

	(void)state;

	mysql = mysql_init(NULL);
	assert_non_null(mysql);

	mysql_options(mysql, MYSQL_OPT_CONNECT_TIMEOUT, (const void *)&timeout);

	/*
	 * Connect to a non-routable address on an unlikely port.
	 * This should fail without hanging.
	 */
	assert_null(mysql_real_connect(mysql, "192.0.2.1", "bad_user",
		"bad_pass", "no_db", 33999, NULL, 0));

	mysql_close(mysql);
}

/* ----------------------------------------------------------------
 * Test: reconnect after mysql_close simulates connection drop
 * ---------------------------------------------------------------- */
static void test_reconnect_after_close(void **state) {
	MYSQL *mysql;
	MYSQL *mysql2;

	(void)state;

	mysql = test_db_connect();
	assert_non_null(mysql);
	assert_true(mysql_ping(mysql) == 0);

	/* simulate a connection drop */
	mysql_close(mysql);

	/* re-establish from scratch (spine pattern) */
	mysql2 = test_db_connect();
	assert_non_null(mysql2);
	assert_true(mysql_ping(mysql2) == 0);

	/* free only the second handle; first was already closed */
	test_db_disconnect(mysql2);
}

/* ----------------------------------------------------------------
 * Test: db_escape handles NULL input without crash
 * ---------------------------------------------------------------- */
static void test_db_escape_null(void **state) {
	MYSQL *mysql = (MYSQL *)*state;
	char output[256];

	memset(output, 'X', sizeof(output));

	/* NULL input should be a no-op; output unchanged */
	db_escape(mysql, output, sizeof(output), NULL);

	/* output should still start with 'X' since db_escape returns early */
	assert_int_equal(output[0], 'X');
}

/* ----------------------------------------------------------------
 * Test: db_escape handles empty string
 * ---------------------------------------------------------------- */
static void test_db_escape_empty(void **state) {
	MYSQL *mysql = (MYSQL *)*state;
	char output[256];

	memset(output, 0, sizeof(output));
	db_escape(mysql, output, sizeof(output), "");
	assert_string_equal(output, "");
}

/* ----------------------------------------------------------------
 * Test: db_escape handles special characters
 * ---------------------------------------------------------------- */
static void test_db_escape_special_chars(void **state) {
	MYSQL *mysql = (MYSQL *)*state;
	char output[256];

	memset(output, 0, sizeof(output));
	db_escape(mysql, output, sizeof(output), "it's a \"test\"\n\\done");

	/* escaped output must contain the backslash-escaped single quote */
	assert_non_null(strstr(output, "\\'"));
	/* escaped output must have a backslash before the double quote */
	assert_non_null(strstr(output, "\\\""));
}

/* ----------------------------------------------------------------
 * Test: db_escape handles oversized input by truncating
 * ---------------------------------------------------------------- */
static void test_db_escape_oversized(void **state) {
	MYSQL *mysql = (MYSQL *)*state;
	char big_input[4096];
	char output[128];

	memset(big_input, 'A', sizeof(big_input) - 1);
	big_input[sizeof(big_input) - 1] = '\0';

	memset(output, 0, sizeof(output));
	db_escape(mysql, output, sizeof(output), big_input);

	/* output should be populated and shorter than the full input */
	assert_true(strlen(output) > 0);
	assert_true(strlen(output) < sizeof(big_input));
}

/* ----------------------------------------------------------------
 * Test: db_free_result handles NULL without crash
 *
 * mysql_free_result(NULL) is documented as a no-op, but we verify
 * spine's wrapper does not add any dereference before the call.
 * ---------------------------------------------------------------- */
static void test_db_free_result_null(void **state) {
	(void)state;

	/* should not segfault */
	db_free_result(NULL);
}

/* ----------------------------------------------------------------
 * Test: db_column_exists returns TRUE for existing column
 * ---------------------------------------------------------------- */
static void test_db_column_exists_true(void **state) {
	MYSQL *mysql = (MYSQL *)*state;
	int result;

	result = db_column_exists(mysql, LOCAL, "settings", "name");
	assert_int_equal(result, TRUE);
}

/* ----------------------------------------------------------------
 * Test: db_column_exists returns FALSE for missing column
 * ---------------------------------------------------------------- */
static void test_db_column_exists_false(void **state) {
	MYSQL *mysql = (MYSQL *)*state;
	int result;

	result = db_column_exists(mysql, LOCAL, "settings", "nonexistent_col_xyz");
	assert_int_equal(result, FALSE);
}

/* ----------------------------------------------------------------
 * main
 * ---------------------------------------------------------------- */
int main(void) {
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_connect_valid),
		cmocka_unit_test(test_connect_bad_host),
		cmocka_unit_test(test_reconnect_after_close),
		cmocka_unit_test(test_db_escape_null),
		cmocka_unit_test(test_db_escape_empty),
		cmocka_unit_test(test_db_escape_special_chars),
		cmocka_unit_test(test_db_escape_oversized),
		cmocka_unit_test(test_db_free_result_null),
		cmocka_unit_test(test_db_column_exists_true),
		cmocka_unit_test(test_db_column_exists_false),
	};

	return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
