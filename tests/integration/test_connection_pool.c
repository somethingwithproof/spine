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
 | Integration tests for the MySQL connection pool under single and        |
 | multi-threaded access patterns.                                         |
 +-------------------------------------------------------------------------+
*/

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
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

#define TEST_POOL_SIZE 4

static void setup_spine_db_config(void) {
	const char *host;
	const char *user;
	const char *pass;
	const char *db;

	host = test_env_or_default(TEST_DB_HOST_ENV, TEST_DB_HOST_DEFAULT);
	user = test_env_or_default(TEST_DB_USER_ENV, TEST_DB_USER_DEFAULT);
	pass = test_env_or_default(TEST_DB_PASS_ENV, TEST_DB_PASS_DEFAULT);
	db   = test_env_or_default(TEST_DB_NAME_ENV, TEST_DB_NAME_DEFAULT);

	strncpy(set.db_host, host, BUFSIZE - 1);
	strncpy(set.db_user, user, BUFSIZE - 1);
	strncpy(set.db_pass, pass, BUFSIZE - 1);
	strncpy(set.db_db, db, BUFSIZE - 1);
	set.db_port = (unsigned int)test_env_port();
}

static int group_setup(void **state) {
	MYSQL *verify_conn;

	memset(&set, 0, sizeof(set));
	set.log_level = POLLER_VERBOSITY_NONE;
	set.log_destination = LOGDEST_STDOUT;
	set.poller_id = 1;
	set.threads = TEST_POOL_SIZE;

	init_mutexes();
	setup_spine_db_config();

	/* verify DB is accessible before running pool tests */
	verify_conn = test_db_connect();
	if (verify_conn == NULL) {
		fprintf(stderr, "SKIP: cannot connect to test database\n");
		return -1;
	}
	test_db_disconnect(verify_conn);

	/* allocate the pool array (spine normally does this in main()) */
	db_pool_local = (pool_t *)calloc(TEST_POOL_SIZE, sizeof(pool_t));
	if (db_pool_local == NULL) {
		fprintf(stderr, "FATAL: calloc for pool failed\n");
		return -1;
	}

	/* create the connection pool using spine's own function */
	db_create_connection_pool(LOCAL);

	*state = NULL;
	return 0;
}

static int group_teardown(void **state) {
	(void)state;

	db_close_connection_pool(LOCAL);
	db_pool_local = NULL;

	return 0;
}

/* ----------------------------------------------------------------
 * Test: get a connection from the pool and release it
 * ---------------------------------------------------------------- */
static void test_get_and_release(void **state) {
	pool_t *conn;

	(void)state;

	conn = db_get_connection(LOCAL);
	assert_non_null(conn);
	assert_int_equal(conn->free, FALSE);
	assert_true(conn->id >= 0 && conn->id < TEST_POOL_SIZE);

	/* verify the connection is alive */
	assert_int_equal(mysql_ping(&conn->mysql), 0);

	db_release_connection(LOCAL, conn->id);
}

/* ----------------------------------------------------------------
 * Test: exhaust the pool, then verify all are checked out
 * ---------------------------------------------------------------- */
static void test_exhaust_pool(void **state) {
	pool_t *conns[TEST_POOL_SIZE];
	int i;

	(void)state;

	/* check out all connections */
	for (i = 0; i < TEST_POOL_SIZE; i++) {
		conns[i] = db_get_connection(LOCAL);
		assert_non_null(conns[i]);
		assert_int_equal(conns[i]->free, FALSE);
	}

	/* next request should return NULL (pool exhausted) */
	assert_null(db_get_connection(LOCAL));

	/* release all connections */
	for (i = 0; i < TEST_POOL_SIZE; i++) {
		db_release_connection(LOCAL, conns[i]->id);
	}

	/* pool should be usable again */
	conns[0] = db_get_connection(LOCAL);
	assert_non_null(conns[0]);
	db_release_connection(LOCAL, conns[0]->id);
}

/* ----------------------------------------------------------------
 * Test: connections returned to pool are reusable
 * ---------------------------------------------------------------- */
static void test_connection_reuse(void **state) {
	pool_t *conn1;
	pool_t *conn2;
	int first_id;

	(void)state;

	conn1 = db_get_connection(LOCAL);
	assert_non_null(conn1);
	first_id = conn1->id;

	/* run a query to verify it works */
	assert_int_equal(mysql_ping(&conn1->mysql), 0);

	db_release_connection(LOCAL, conn1->id);

	/* get another connection; should be the same one (pool scans from 0) */
	conn2 = db_get_connection(LOCAL);
	assert_non_null(conn2);

	/*
	 * The pool scans linearly from id 0, so we should get the same
	 * connection back (or at least a valid one).
	 */
	assert_int_equal(conn2->id, first_id);
	assert_int_equal(mysql_ping(&conn2->mysql), 0);

	db_release_connection(LOCAL, conn2->id);
}

/* ----------------------------------------------------------------
 * Test: each pool connection can execute a query independently
 * ---------------------------------------------------------------- */
static void test_pool_query_each(void **state) {
	pool_t *conns[TEST_POOL_SIZE];
	int i;

	(void)state;

	for (i = 0; i < TEST_POOL_SIZE; i++) {
		MYSQL_RES *result;

		conns[i] = db_get_connection(LOCAL);
		assert_non_null(conns[i]);

		result = db_query(&conns[i]->mysql, LOCAL, "SELECT 1");
		assert_non_null(result);
		db_free_result(result);
	}

	for (i = 0; i < TEST_POOL_SIZE; i++) {
		db_release_connection(LOCAL, conns[i]->id);
	}
}

/* ----------------------------------------------------------------
 * Multi-threaded stress test
 * ---------------------------------------------------------------- */
#define STRESS_ITERATIONS 20

/* per-thread result: 0 = success, nonzero = failure count */
static int thread_results[TEST_POOL_SIZE];

static void *pool_stress_worker(void *arg) {
	int thread_idx = *(int *)arg;
	int failures = 0;
	int i;

	for (i = 0; i < STRESS_ITERATIONS; i++) {
		pool_t *conn = db_get_connection(LOCAL);
		if (conn == NULL) {
			/*
			 * Pool exhaustion under contention is expected.
			 * Sleep briefly and retry.
			 */
			usleep(1000);
			continue;
		}

		if (mysql_ping(&conn->mysql) != 0) {
			failures++;
		}

		db_release_connection(LOCAL, conn->id);
	}

	thread_results[thread_idx] = failures;
	return NULL;
}

/* ----------------------------------------------------------------
 * Test: multi-threaded checkout/release stress test
 * ---------------------------------------------------------------- */
static void test_multithreaded_stress(void **state) {
	pthread_t threads[TEST_POOL_SIZE];
	int thread_ids[TEST_POOL_SIZE];
	int i;
	int total_failures = 0;

	(void)state;

	memset(thread_results, 0, sizeof(thread_results));

	for (i = 0; i < TEST_POOL_SIZE; i++) {
		thread_ids[i] = i;
		assert_int_equal(
			pthread_create(&threads[i], NULL, pool_stress_worker, &thread_ids[i]),
			0);
	}

	for (i = 0; i < TEST_POOL_SIZE; i++) {
		pthread_join(threads[i], NULL);
		total_failures += thread_results[i];
	}

	assert_int_equal(total_failures, 0);
}

/* ----------------------------------------------------------------
 * Test: pool connections survive a simple query workload
 * ---------------------------------------------------------------- */
static void test_pool_survives_workload(void **state) {
	pool_t *conn;
	int i;

	(void)state;

	conn = db_get_connection(LOCAL);
	assert_non_null(conn);

	/* run multiple queries on the same pooled connection */
	for (i = 0; i < 50; i++) {
		MYSQL_RES *result;
		char query[128];

		snprintf(query, sizeof(query), "SELECT %d AS val", i);
		result = db_query(&conn->mysql, LOCAL, query);
		assert_non_null(result);
		db_free_result(result);
	}

	db_release_connection(LOCAL, conn->id);
}

/* ----------------------------------------------------------------
 * main
 * ---------------------------------------------------------------- */
int main(void) {
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_get_and_release),
		cmocka_unit_test(test_exhaust_pool),
		cmocka_unit_test(test_connection_reuse),
		cmocka_unit_test(test_pool_query_each),
		cmocka_unit_test(test_multithreaded_stress),
		cmocka_unit_test(test_pool_survives_workload),
	};

	return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
