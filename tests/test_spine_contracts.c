/*
 * Copyright (C) 2004-2026 The Cacti Group
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License
 * for more details.
 */
#include "common.h"
#include "spine.h"
#include <stdint.h>

static void test_keyword_roundtrips(void) {
	static const struct { const char *word; int value; } levels[] = {
		{"NONE", POLLER_VERBOSITY_NONE}, {"LOW", POLLER_VERBOSITY_LOW},
		{"MEDIUM", POLLER_VERBOSITY_MEDIUM}, {"HIGH", POLLER_VERBOSITY_HIGH},
		{"DEBUG", POLLER_VERBOSITY_DEBUG}
	}, destinations[] = {
		{"FILE", LOGDEST_FILE}, {"SYSLOG", LOGDEST_SYSLOG},
		{"BOTH", LOGDEST_BOTH}, {"STDOUT", LOGDEST_STDOUT}
	}, actions[] = {
		{"SNMP", POLLER_ACTION_SNMP}, {"SCRIPT", POLLER_ACTION_SCRIPT},
		{"PHPSCRIPT", POLLER_ACTION_PHP_SCRIPT_SERVER},
		{"SNMP_CT", POLLER_ACTION_SNMP_COUNT}, {"SCRIPT_CT", POLLER_ACTION_SCRIPT_COUNT},
		{"PHPSCRIPT_CT", POLLER_ACTION_PHP_SCRIPT_SERVER_COUNT}
	};
	for (size_t i = 0; i < sizeof(levels)/sizeof(levels[0]); i++) {
		assert(parse_log_level(levels[i].word, -1) == levels[i].value);
		assert(strcmp(printable_log_level(levels[i].value), levels[i].word) == 0);
	}
	for (size_t i = 0; i < sizeof(destinations)/sizeof(destinations[0]); i++) {
		assert(parse_logdest(destinations[i].word, -1) == destinations[i].value);
		assert(strcmp(printable_logdest(destinations[i].value), destinations[i].word) == 0);
	}
	for (size_t i = 0; i < sizeof(actions)/sizeof(actions[0]); i++) {
		assert(parse_action(actions[i].word, -1) == actions[i].value);
		assert(strcmp(printable_action(actions[i].value), actions[i].word) == 0);
	}
	assert(parse_log_level("dEbUg", -1) == POLLER_VERBOSITY_DEBUG);
	assert(parse_logdest("sYsLoG", -1) == LOGDEST_SYSLOG);
	assert(parse_action("pHpScRiPt_Ct", -1) == POLLER_ACTION_PHP_SCRIPT_SERVER_COUNT);
	assert(parse_log_level("42", -1) == 42);
	assert(parse_logdest("007", -1) == 7);
	assert(parse_action("0", -1) == 0);
	assert(parse_log_level("", 97) == 97);
	assert(parse_log_level("unknown", 97) == 97);
	assert(parse_logdest("-1", 98) == 98);
	assert(parse_action(" 2", 99) == 99);
	assert(parse_action("2x", 99) == 99);
	assert(strcmp(printable_log_level(-1), "-unknown-") == 0);
	assert(strcmp(printable_logdest(-1), "-unknown-") == 0);
	assert(strcmp(printable_action(-1), "-unknown-") == 0);
}

static void *try_busy_lock(void *argument) {
	int lock = *(int *)argument;
	int result = thread_mutex_trylock(lock);
	if (result == 0) thread_mutex_unlock(lock);
	return (void *)(intptr_t)result;
}

static void test_lock_contracts(void) {
	static const int locks[] = {LOCK_SNMP, LOCK_SETEUID, LOCK_GHBN, LOCK_POOL, LOCK_PHP,
		LOCK_PHP_PROC_0, LOCK_PHP_PROC_1, LOCK_PHP_PROC_2, LOCK_PHP_PROC_3,
		LOCK_PHP_PROC_4, LOCK_PHP_PROC_5, LOCK_PHP_PROC_6, LOCK_PHP_PROC_7,
		LOCK_PHP_PROC_8, LOCK_PHP_PROC_9, LOCK_PHP_PROC_10, LOCK_PHP_PROC_11,
		LOCK_PHP_PROC_12, LOCK_PHP_PROC_13, LOCK_PHP_PROC_14, LOCK_THDET, LOCK_HOST_TIME};
	init_mutexes();
	init_mutexes(); /* pthread_once must preserve the already-created locks. */
	for (size_t i = 0; i < sizeof(locks)/sizeof(locks[0]); i++) {
		assert(get_lock(locks[i]) != NULL && get_cond(locks[i]) != NULL && get_attr(locks[i]) != NULL);
		assert(get_lock(locks[i]) == get_lock(locks[i]));
		for (size_t j = 0; j < i; j++) {
			assert(get_lock(locks[i]) != get_lock(locks[j]));
			assert(get_cond(locks[i]) != get_cond(locks[j]));
			assert(get_attr(locks[i]) != get_attr(locks[j]));
		}
		assert(thread_mutex_trylock(locks[i]) == 0);
		pthread_t competitor;
		int lock = locks[i];
		assert(pthread_create(&competitor, NULL, try_busy_lock, &lock) == 0);
		void *result;
		assert(pthread_join(competitor, &result) == 0);
		assert((intptr_t)result == EBUSY);
		thread_mutex_unlock(locks[i]);
		assert(thread_mutex_trylock(locks[i]) == 0);
		thread_mutex_unlock(locks[i]);
	}
	assert(get_lock(-1) == NULL && get_cond(-1) == NULL && get_attr(-1) == NULL);
	assert(get_lock(1) == NULL && get_cond(1) == NULL && get_attr(1) == NULL);
}

static pthread_mutex_t distribution_mutex = PTHREAD_MUTEX_INITIALIZER;
static int distribution[MAX_PHP_SERVERS];
static void *next_php_process(void *unused) {
	(void)unused;
	for (int i = 0; i < 300; i++) {
		int process = php_get_process();
		assert(process >= 0 && process < 3);
		assert(pthread_mutex_lock(&distribution_mutex) == 0);
		distribution[process]++;
		assert(pthread_mutex_unlock(&distribution_mutex) == 0);
	}
	return NULL;
}

static void test_php_roundrobin(void) {
	int previous_count = set.php_servers;
	int previous_current = set.php_current_server;
	set.php_servers = MAX_PHP_SERVERS;
	set.php_current_server = 0;
	for (int i = 0; i < 2 * MAX_PHP_SERVERS; i++) assert(php_get_process() == i % MAX_PHP_SERVERS);
	set.php_servers = 1;
	set.php_current_server = MAX_PHP_SERVERS;
	assert(php_get_process() == 0 && php_get_process() == 0);
	set.php_servers = 3;
	set.php_current_server = 0;
	memset(distribution, 0, sizeof(distribution));
	pthread_t workers[4];
	for (size_t i = 0; i < 4; i++) assert(pthread_create(&workers[i], NULL, next_php_process, NULL) == 0);
	for (size_t i = 0; i < 4; i++) assert(pthread_join(workers[i], NULL) == 0);
	for (int i = 0; i < 3; i++) assert(distribution[i] == 400);
	assert(set.php_current_server == 3);
	set.php_servers = previous_count;
	set.php_current_server = previous_current;
}

static void test_legacy_ip_predicate(void) {
	/* This unused legacy helper recognizes characters, not IP syntax. */
	assert(is_ipaddress("127.0.0.1"));
	assert(is_ipaddress("::1"));
	assert(is_ipaddress(""));
	assert(is_ipaddress("999.999.999.999"));
	assert(is_ipaddress(".:"));
	assert(!is_ipaddress("example.org"));
	assert(!is_ipaddress("2001:db8::1"));
	assert(!is_ipaddress("127.0.0.1 "));
	assert(!is_ipaddress("127.0.0.1/8"));
}

static void test_multipart_boundaries(void) {
	static const struct { const char *input; int expected; } cases[] = {
		{NULL, FALSE}, {"", FALSE}, {"123", FALSE}, {"hello", FALSE},
		{"a:1", TRUE}, {"a!1", TRUE}, {":", TRUE}, {"!", TRUE},
		{"a:1 b:2", TRUE}, {"a!1 b!2", TRUE}, {"a:1 b!2", TRUE},
		{"a:1  b:2", FALSE}, {" a:1", FALSE}, {"a:1 ", FALSE},
		{" a:1 b:2 ", FALSE}, {"a:1 b:2 c:3", TRUE},
		{"a::1 b:2", FALSE}, {"a::1", TRUE},
		{"a:1\tb:2", TRUE}, {"a:1\nb!2", TRUE},
		{"a:1\t b!2", TRUE}, {"a:1 b:2\n c!3", TRUE},
		{"a:1 \t b:2", FALSE}, {"a  b", FALSE}
	};
	for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) assert(is_multipart_output(cases[i].input) == cases[i].expected);
}

void test_additional_contracts(void) {
	test_keyword_roundtrips();
	test_lock_contracts();
	test_php_roundrobin();
	test_legacy_ip_predicate();
	test_multipart_boundaries();
}

void test_additional_database_contracts(MYSQL *mysql) {
	assert(db_column_exists(mysql, LOCAL, "host", "id") == TRUE);
	assert(db_column_exists(mysql, LOCAL, "host", "regression_missing_column") == FALSE);
	/* Only kill a connection created and owned by this test. */
	MYSQL victim;
	db_connect(LOCAL, &victim);
	unsigned long original = mysql_thread_id(&victim);
	char query[100];
	snprintf(query, sizeof(query), "KILL CONNECTION %lu", original);
	assert(mysql_query(mysql, query) == 0);
	assert(db_reconnect(&victim, 2006, "regression_owned_session") == TRUE);
	assert(mysql_thread_id(&victim) != original);
	assert(mysql_query(&victim, "SELECT 123") == 0);
	MYSQL_RES *result = mysql_store_result(&victim);
	assert(result != NULL);
	MYSQL_ROW row = mysql_fetch_row(result);
	assert(row != NULL && strcmp(row[0], "123") == 0);
	mysql_free_result(result);
	/* An already-healthy session does not count as a new reconnection. */
	assert(db_reconnect(&victim, 2006, "regression_healthy_session") == FALSE);
	db_disconnect(&victim);
}
