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
 | Integration tests for SQL buffer operations against real MySQL.         |
 | Tests bulk INSERT construction, execution, overflow handling, and       |
 | poller_output row formatting.                                           |
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

/* helper: count rows in poller_output */
static int count_poller_output_rows(MYSQL *mysql) {
	MYSQL_RES *result;
	MYSQL_ROW row;
	int count = 0;

	result = db_query(mysql, LOCAL, "SELECT COUNT(*) FROM poller_output");
	if (result != NULL) {
		row = mysql_fetch_row(result);
		if (row != NULL && row[0] != NULL) {
			count = atoi(row[0]);
		}
		db_free_result(result);
	}
	return count;
}

/* ----------------------------------------------------------------
 * Test: build and execute a bulk INSERT into poller_output
 * ---------------------------------------------------------------- */
static void test_bulk_insert(void **state) {
	MYSQL *mysql = (MYSQL *)*state;
	char query[LRG_BUFSIZE];
	int result;
	int rows_before;
	int rows_after;

	test_db_exec(mysql, "DELETE FROM poller_output");
	rows_before = count_poller_output_rows(mysql);

	/* build a bulk insert with multiple value tuples */
	snprintf(query, sizeof(query),
		"INSERT INTO poller_output (local_data_id, rrd_name, time, output) VALUES"
		" (100, 'traffic_in', NOW(), '12345'),"
		" (101, 'traffic_out', NOW(), '67890'),"
		" (102, 'errors', NOW(), '0')");

	result = db_insert(mysql, LOCAL, query);
	assert_int_equal(result, TRUE);

	rows_after = count_poller_output_rows(mysql);
	assert_int_equal(rows_after - rows_before, 3);

	test_db_exec(mysql, "DELETE FROM poller_output");
}

/* ----------------------------------------------------------------
 * Test: single row insert and verify content
 * ---------------------------------------------------------------- */
static void test_single_insert_verify(void **state) {
	MYSQL *mysql = (MYSQL *)*state;
	MYSQL_RES *result;
	MYSQL_ROW row;
	int ret;

	test_db_exec(mysql, "DELETE FROM poller_output");

	ret = db_insert(mysql, LOCAL,
		"INSERT INTO poller_output (local_data_id, rrd_name, time, output)"
		" VALUES (200, 'cpu_usage', NOW(), '42.5')");
	assert_int_equal(ret, TRUE);

	result = db_query(mysql, LOCAL,
		"SELECT output FROM poller_output WHERE local_data_id = 200");
	assert_non_null(result);

	row = mysql_fetch_row(result);
	assert_non_null(row);
	assert_string_equal(row[0], "42.5");

	db_free_result(result);
	test_db_exec(mysql, "DELETE FROM poller_output");
}

/* ----------------------------------------------------------------
 * Test: buffer overflow handling -- query at MAX_MYSQL_BUF_SIZE
 *
 * Build a query that exceeds the normal SQL buffer capacity.
 * Verify it either succeeds (MySQL handles large packets) or
 * fails gracefully without crashing.
 * ---------------------------------------------------------------- */
static void test_buffer_overflow_handling(void **state) {
	MYSQL *mysql = (MYSQL *)*state;
	char *big_query;
	size_t query_len;
	size_t offset;
	int i;
	int ret;

	test_db_exec(mysql, "DELETE FROM poller_output");

	/*
	 * Build a query that approaches MAX_MYSQL_BUF_SIZE.
	 * Use many small inserts to fill the buffer.
	 */
	query_len = MAX_MYSQL_BUF_SIZE + RESULTS_BUFFER;
	big_query = (char *)calloc(query_len, 1);
	assert_non_null(big_query);

	offset = (size_t)snprintf(big_query, query_len,
		"INSERT INTO poller_output (local_data_id, rrd_name, time, output) VALUES");

	for (i = 0; i < 500 && offset < (query_len - 256); i++) {
		int written;
		if (i > 0) {
			big_query[offset++] = ',';
		}
		written = snprintf(big_query + offset, query_len - offset,
			" (%d, 'rrd_%d', NOW(), '%d')", 1000 + i, i, i * 100);
		if (written < 0) {
			break;
		}
		offset += (size_t)written;
	}

	/*
	 * Execute the large query. The result depends on MySQL's
	 * max_allowed_packet, but spine should not crash either way.
	 */
	ret = db_insert(mysql, LOCAL, big_query);

	if (ret == TRUE) {
		/* verify at least some rows were inserted */
		assert_true(count_poller_output_rows(mysql) > 0);
	}
	/* if ret == FALSE, that is acceptable (buffer too large) */

	free(big_query);
	test_db_exec(mysql, "DELETE FROM poller_output");
}

/* ----------------------------------------------------------------
 * Test: poller_output row format matches expected SQL pattern
 *
 * Spine builds result rows as:
 *   (local_data_id, 'rrd_name', FROM_UNIXTIME(ts), 'output')
 * Verify the format produces valid SQL when inserted.
 * ---------------------------------------------------------------- */
static void test_poller_output_row_format(void **state) {
	MYSQL *mysql = (MYSQL *)*state;
	char result_string[RESULTS_BUFFER + SMALL_BUFSIZE];
	char query[LRG_BUFSIZE];
	int ret;
	int rows;

	test_db_exec(mysql, "DELETE FROM poller_output");

	/*
	 * Simulate the format spine uses in poller.c line ~1822:
	 *   snprintf(result_string, ..., " (%i, '%s', FROM_UNIXTIME(%s), '%s')", ...)
	 */
	snprintf(result_string, sizeof(result_string),
		" (%d, '%s', FROM_UNIXTIME(%ld), '%s')",
		300, "bandwidth", (long)1711234567, "99999");

	snprintf(query, sizeof(query),
		"INSERT INTO poller_output (local_data_id, rrd_name, time, output) VALUES%s",
		result_string);

	ret = db_insert(mysql, LOCAL, query);
	assert_int_equal(ret, TRUE);

	rows = count_poller_output_rows(mysql);
	assert_int_equal(rows, 1);

	test_db_exec(mysql, "DELETE FROM poller_output");
}

/* ----------------------------------------------------------------
 * Test: escaped values in INSERT do not cause SQL errors
 * ---------------------------------------------------------------- */
static void test_escaped_value_insert(void **state) {
	MYSQL *mysql = (MYSQL *)*state;
	char escaped_output[512];
	char query[LRG_BUFSIZE];
	MYSQL_RES *result;
	MYSQL_ROW row;
	int ret;

	test_db_exec(mysql, "DELETE FROM poller_output");

	/* escape a value containing special characters */
	db_escape(mysql, escaped_output, sizeof(escaped_output),
		"test's \"value\" with\\backslash");

	snprintf(query, sizeof(query),
		"INSERT INTO poller_output (local_data_id, rrd_name, time, output)"
		" VALUES (400, 'special', NOW(), '%s')", escaped_output);

	ret = db_insert(mysql, LOCAL, query);
	assert_int_equal(ret, TRUE);

	/* read it back and verify the value round-trips */
	result = db_query(mysql, LOCAL,
		"SELECT output FROM poller_output WHERE local_data_id = 400");
	assert_non_null(result);

	row = mysql_fetch_row(result);
	assert_non_null(row);
	assert_non_null(strstr(row[0], "test's"));
	assert_non_null(strstr(row[0], "backslash"));

	db_free_result(result);
	test_db_exec(mysql, "DELETE FROM poller_output");
}

/* ----------------------------------------------------------------
 * Test: db_insert with SQL_readonly flag is a no-op
 * ---------------------------------------------------------------- */
static void test_insert_readonly_mode(void **state) {
	MYSQL *mysql = (MYSQL *)*state;
	int ret;
	int rows;

	test_db_exec(mysql, "DELETE FROM poller_output");

	set.SQL_readonly = TRUE;

	ret = db_insert(mysql, LOCAL,
		"INSERT INTO poller_output (local_data_id, rrd_name, time, output)"
		" VALUES (500, 'readonly', NOW(), 'should_not_appear')");
	assert_int_equal(ret, TRUE);

	/* row should NOT actually exist */
	rows = count_poller_output_rows(mysql);
	assert_int_equal(rows, 0);

	set.SQL_readonly = FALSE;
}

/* ----------------------------------------------------------------
 * main
 * ---------------------------------------------------------------- */
int main(void) {
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_bulk_insert),
		cmocka_unit_test(test_single_insert_verify),
		cmocka_unit_test(test_buffer_overflow_handling),
		cmocka_unit_test(test_poller_output_row_format),
		cmocka_unit_test(test_escaped_value_insert),
		cmocka_unit_test(test_insert_readonly_mode),
	};

	return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
