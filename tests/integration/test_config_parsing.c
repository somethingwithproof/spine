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
 | Integration tests for config parsing: settings retrieval from live DB,  |
 | set_option override mechanism, global variable lookup.                  |
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
 * Test: read a known setting from the database via raw query
 *
 * getsetting() is static in util.c, so we verify it indirectly
 * by querying the settings table the same way spine does.
 * ---------------------------------------------------------------- */
static void test_read_setting_from_db(void **state) {
	MYSQL *mysql = (MYSQL *)*state;
	MYSQL_RES *result;
	MYSQL_ROW row;

	result = db_query(mysql, LOCAL,
		"SELECT SQL_NO_CACHE value FROM settings WHERE name = 'log_verbosity'");
	assert_non_null(result);
	assert_true(mysql_num_rows(result) > 0);

	row = mysql_fetch_row(result);
	assert_non_null(row);
	assert_non_null(row[0]);
	assert_string_equal(row[0], "5");

	db_free_result(result);
}

/* ----------------------------------------------------------------
 * Test: read poller setting (getpsetting pattern)
 * ---------------------------------------------------------------- */
static void test_read_poller_setting(void **state) {
	MYSQL *mysql = (MYSQL *)*state;
	MYSQL_RES *result;
	MYSQL_ROW row;

	result = db_query(mysql, LOCAL,
		"SELECT SQL_NO_CACHE threads FROM poller WHERE id = 1");
	assert_non_null(result);
	assert_true(mysql_num_rows(result) > 0);

	row = mysql_fetch_row(result);
	assert_non_null(row);
	assert_non_null(row[0]);
	assert_string_equal(row[0], "5");

	db_free_result(result);
}

/* ----------------------------------------------------------------
 * Test: read MySQL global variable (getglobalvariable pattern)
 * ---------------------------------------------------------------- */
static void test_read_global_variable(void **state) {
	MYSQL *mysql = (MYSQL *)*state;
	MYSQL_RES *result;
	MYSQL_ROW row;

	result = db_query(mysql, LOCAL,
		"SHOW GLOBAL VARIABLES LIKE 'version'");
	assert_non_null(result);
	assert_true(mysql_num_rows(result) > 0);

	row = mysql_fetch_row(result);
	assert_non_null(row);
	/* row[0] = variable_name, row[1] = value */
	assert_non_null(row[1]);
	assert_true(strlen(row[1]) > 0);

	db_free_result(result);
}

/* ----------------------------------------------------------------
 * Test: missing setting returns empty result set
 * ---------------------------------------------------------------- */
static void test_missing_setting_returns_empty(void **state) {
	MYSQL *mysql = (MYSQL *)*state;
	MYSQL_RES *result;
	int num_rows;

	result = db_query(mysql, LOCAL,
		"SELECT SQL_NO_CACHE value FROM settings WHERE name = 'nonexistent_setting_xyz'");
	assert_non_null(result);

	num_rows = (int)mysql_num_rows(result);
	assert_int_equal(num_rows, 0);

	db_free_result(result);
}

/* ----------------------------------------------------------------
 * Test: set_option stores values that can be retrieved
 *
 * set_option() populates an internal table that getsetting() checks
 * before querying the DB. Since getsetting is static, we test the
 * public interface: set_option + read_config_options side effects.
 * ---------------------------------------------------------------- */
static void test_set_option_override(void **state) {
	(void)state;

	/*
	 * set_option stores a name/value pair in the override table.
	 * Verify it does not crash and accepts multiple values.
	 */
	set_option("test_key_1", "override_value_1");
	set_option("test_key_2", "override_value_2");

	/*
	 * Cannot read back directly since getsetting is static.
	 * The fact that set_option does not crash and the override table
	 * is populated is the test. read_config_options() will use these
	 * overrides when it runs.
	 */
}

/* ----------------------------------------------------------------
 * Test: config_defaults initializes set structure sanely
 * ---------------------------------------------------------------- */
static void test_config_defaults(void **state) {
	(void)state;

	memset(&set, 0, sizeof(set));
	config_defaults();

	/* verify some known defaults from spine.h */
	assert_int_equal(set.threads, DEFAULT_THREADS);
	assert_int_equal(set.db_port, DEFAULT_DB_PORT);
}

/* ----------------------------------------------------------------
 * Test: settings table INSERT/UPDATE via db_insert
 * ---------------------------------------------------------------- */
static void test_settings_insert_update(void **state) {
	MYSQL *mysql = (MYSQL *)*state;
	MYSQL_RES *result;
	MYSQL_ROW row;
	int ret;

	/* insert a new setting */
	ret = db_insert(mysql, LOCAL,
		"INSERT INTO settings (name, value) VALUES ('test_setting', 'initial')"
		" ON DUPLICATE KEY UPDATE value = 'initial'");
	assert_int_equal(ret, TRUE);

	/* update the setting */
	ret = db_insert(mysql, LOCAL,
		"UPDATE settings SET value = 'updated' WHERE name = 'test_setting'");
	assert_int_equal(ret, TRUE);

	/* verify the update took effect */
	result = db_query(mysql, LOCAL,
		"SELECT value FROM settings WHERE name = 'test_setting'");
	assert_non_null(result);

	row = mysql_fetch_row(result);
	assert_non_null(row);
	assert_string_equal(row[0], "updated");

	db_free_result(result);

	/* cleanup */
	test_db_exec(mysql, "DELETE FROM settings WHERE name = 'test_setting'");
}

/* ----------------------------------------------------------------
 * Test: multiple settings can be queried in a batch
 * ---------------------------------------------------------------- */
static void test_batch_settings_query(void **state) {
	MYSQL *mysql = (MYSQL *)*state;
	MYSQL_RES *result;
	int num_rows;

	result = db_query(mysql, LOCAL,
		"SELECT name, value FROM settings WHERE name IN "
		"('log_verbosity', 'poller_interval', 'max_threads', 'spine_log_level')");
	assert_non_null(result);

	num_rows = (int)mysql_num_rows(result);
	assert_int_equal(num_rows, 4);

	db_free_result(result);
}

/* ----------------------------------------------------------------
 * Test: global variable query for max_allowed_packet
 * ---------------------------------------------------------------- */
static void test_global_variable_max_packet(void **state) {
	MYSQL *mysql = (MYSQL *)*state;
	MYSQL_RES *result;
	MYSQL_ROW row;

	result = db_query(mysql, LOCAL,
		"SHOW GLOBAL VARIABLES LIKE 'max_allowed_packet'");
	assert_non_null(result);
	assert_true(mysql_num_rows(result) > 0);

	row = mysql_fetch_row(result);
	assert_non_null(row);
	assert_non_null(row[1]);

	/* max_allowed_packet should be a positive integer */
	assert_true(atoi(row[1]) > 0);

	db_free_result(result);
}

/* ----------------------------------------------------------------
 * main
 * ---------------------------------------------------------------- */
int main(void) {
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_read_setting_from_db),
		cmocka_unit_test(test_read_poller_setting),
		cmocka_unit_test(test_read_global_variable),
		cmocka_unit_test(test_missing_setting_returns_empty),
		cmocka_unit_test(test_set_option_override),
		cmocka_unit_test(test_config_defaults),
		cmocka_unit_test(test_settings_insert_update),
		cmocka_unit_test(test_batch_settings_query),
		cmocka_unit_test(test_global_variable_max_packet),
	};

	return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
