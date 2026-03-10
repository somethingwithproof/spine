/*
 ex: set tabstop=4 shiftwidth=4 autoindent:
 +-------------------------------------------------------------------------+
 | Copyright (C) 2004-2026 The Cacti Group                                 |
 |                                                                         |
 | This program is free software; you can redistribute it and/or           |
 | modify it under the terms of the GNU Lesser General Public              |
 | License as published by the Free Software Foundation; either            |
 | version 2.1 of the License, or (at your option) any later version.     |
 +-------------------------------------------------------------------------+
 | spine: a backend data gatherer for cacti                                |
 +-------------------------------------------------------------------------+
*/

/* Unit tests for spine config parsing (read_spine_config, config_defaults,
 * set_option).  All tests operate on the global `set` struct directly;
 * no live database connection is required.
 */

#include <check.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "spine.h"
#include "util.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/* Write a minimal spine.conf to a temp file and return its path.
 * Caller must unlink() the file after use.
 */
static char *write_temp_conf(const char *content)
{
	char tmpl[] = "/tmp/spine_test_XXXXXX";
	int  fd;
	char *path;

	fd = mkstemp(tmpl);
	if (fd < 0) return NULL;

	write(fd, content, strlen(content));
	close(fd);

	path = strdup(tmpl);
	return path;
}

/* Reset the global set struct to a clean baseline before each test. */
static void setup(void)
{
	memset(&set, 0, sizeof set);
	/* suppress noise on stderr/stdout during tests */
	set.stderr_notty = 1;
	set.stdout_notty = 1;
}

/* ------------------------------------------------------------------ */
/* config_defaults                                                      */
/* ------------------------------------------------------------------ */

START_TEST(test_config_defaults_db_host)
{
	setup();
	config_defaults();
	ck_assert_str_eq(set.db_host, DEFAULT_DB_HOST);
}
END_TEST

START_TEST(test_config_defaults_db_port)
{
	setup();
	config_defaults();
	ck_assert_int_eq(set.db_port, DEFAULT_DB_PORT);
}
END_TEST

START_TEST(test_config_defaults_threads)
{
	setup();
	config_defaults();
	ck_assert_int_eq(set.threads, DEFAULT_THREADS);
}
END_TEST

START_TEST(test_config_defaults_log_destination)
{
	setup();
	config_defaults();
	ck_assert_int_eq(set.log_destination, LOGDEST_FILE);
}
END_TEST

START_TEST(test_config_defaults_rdb_mirrors_db)
{
	/* remote DB defaults must match local DB defaults */
	setup();
	config_defaults();
	ck_assert_str_eq(set.rdb_host, DEFAULT_DB_HOST);
	ck_assert_int_eq(set.rdb_port, DEFAULT_DB_PORT);
}
END_TEST

/* ------------------------------------------------------------------ */
/* read_spine_config -- missing file                                    */
/* ------------------------------------------------------------------ */

START_TEST(test_read_spine_config_missing_file)
{
	int rc;

	setup();
	rc = read_spine_config("/tmp/spine_nonexistent_config_file.conf");
	ck_assert_int_eq(rc, -1);
}
END_TEST

/* ------------------------------------------------------------------ */
/* read_spine_config -- valid directives                                */
/* ------------------------------------------------------------------ */

START_TEST(test_read_spine_config_db_host)
{
	char *path;
	int   rc;

	setup();
	path = write_temp_conf("DB_Host testhost.example.com\n");
	ck_assert_ptr_ne(path, NULL);

	rc = read_spine_config(path);
	ck_assert_int_eq(rc, 0);
	ck_assert_str_eq(set.db_host, "testhost.example.com");

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_read_spine_config_db_port)
{
	char *path;
	int   rc;

	setup();
	path = write_temp_conf("DB_Port 3307\n");
	ck_assert_ptr_ne(path, NULL);

	rc = read_spine_config(path);
	ck_assert_int_eq(rc, 0);
	ck_assert_int_eq(set.db_port, 3307);

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_read_spine_config_db_ssl_flag)
{
	char *path;
	int   rc;

	setup();
	path = write_temp_conf("DB_UseSSL 1\n");
	ck_assert_ptr_ne(path, NULL);

	rc = read_spine_config(path);
	ck_assert_int_eq(rc, 0);
	ck_assert_int_eq(set.db_ssl, 1);

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_read_spine_config_poller_id)
{
	char *path;
	int   rc;

	setup();
	path = write_temp_conf("Poller 3\n");
	ck_assert_ptr_ne(path, NULL);

	rc = read_spine_config(path);
	ck_assert_int_eq(rc, 0);
	ck_assert_int_eq(set.poller_id, 3);

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_read_spine_config_comment_ignored)
{
	char *path;
	int   rc;

	setup();
	/* Line starting with '#' must be skipped; db_host stays empty */
	path = write_temp_conf("# DB_Host shouldbeignored\n");
	ck_assert_ptr_ne(path, NULL);

	rc = read_spine_config(path);
	ck_assert_int_eq(rc, 0);
	ck_assert_str_eq(set.db_host, "");

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_read_spine_config_multiple_directives)
{
	char *path;
	int   rc;
	const char *conf =
		"DB_Host db.example.com\n"
		"DB_Database mydb\n"
		"DB_User spine\n"
		"DB_Port 3308\n";

	setup();
	path = write_temp_conf(conf);
	ck_assert_ptr_ne(path, NULL);

	rc = read_spine_config(path);
	ck_assert_int_eq(rc, 0);
	ck_assert_str_eq(set.db_host, "db.example.com");
	ck_assert_str_eq(set.db_db,   "mydb");
	ck_assert_str_eq(set.db_user, "spine");
	ck_assert_int_eq(set.db_port, 3308);

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_read_spine_config_cacti_log_sets_destination)
{
	char *path;
	int   rc;

	setup();
	path = write_temp_conf("Cacti_Log /var/log/cacti/spine.log\n");
	ck_assert_ptr_ne(path, NULL);

	rc = read_spine_config(path);
	ck_assert_int_eq(rc, 0);
	ck_assert_str_eq(set.path_logfile, "/var/log/cacti/spine.log");
	ck_assert_int_eq(set.logfile_processed, 1);
	ck_assert_int_eq(set.log_destination, LOGDEST_BOTH);

	unlink(path);
	free(path);
}
END_TEST

START_TEST(test_read_spine_config_rdb_host)
{
	char *path;
	int   rc;

	setup();
	path = write_temp_conf("RDB_Host rdb.example.com\n");
	ck_assert_ptr_ne(path, NULL);

	rc = read_spine_config(path);
	ck_assert_int_eq(rc, 0);
	ck_assert_str_eq(set.rdb_host, "rdb.example.com");

	unlink(path);
	free(path);
}
END_TEST

/* ------------------------------------------------------------------ */
/* Suite assembly                                                       */
/* ------------------------------------------------------------------ */

static Suite *config_suite(void)
{
	Suite *s;
	TCase *tc_defaults, *tc_parse;

	s = suite_create("config");

	tc_defaults = tcase_create("config_defaults");
	tcase_add_test(tc_defaults, test_config_defaults_db_host);
	tcase_add_test(tc_defaults, test_config_defaults_db_port);
	tcase_add_test(tc_defaults, test_config_defaults_threads);
	tcase_add_test(tc_defaults, test_config_defaults_log_destination);
	tcase_add_test(tc_defaults, test_config_defaults_rdb_mirrors_db);
	suite_add_tcase(s, tc_defaults);

	tc_parse = tcase_create("read_spine_config");
	tcase_add_test(tc_parse, test_read_spine_config_missing_file);
	tcase_add_test(tc_parse, test_read_spine_config_db_host);
	tcase_add_test(tc_parse, test_read_spine_config_db_port);
	tcase_add_test(tc_parse, test_read_spine_config_db_ssl_flag);
	tcase_add_test(tc_parse, test_read_spine_config_poller_id);
	tcase_add_test(tc_parse, test_read_spine_config_comment_ignored);
	tcase_add_test(tc_parse, test_read_spine_config_multiple_directives);
	tcase_add_test(tc_parse, test_read_spine_config_cacti_log_sets_destination);
	tcase_add_test(tc_parse, test_read_spine_config_rdb_host);
	suite_add_tcase(s, tc_parse);

	return s;
}

int main(void)
{
	Suite   *s  = config_suite();
	SRunner *sr = srunner_create(s);

	srunner_run_all(sr, CK_NORMAL);

	int failed = srunner_ntests_failed(sr);
	srunner_free(sr);

	return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
