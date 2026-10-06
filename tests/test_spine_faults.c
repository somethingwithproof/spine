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
#include <dirent.h>
#include <fcntl.h>

/* GNU ld wraps the actual API boundary. No production test-only code is used. */
extern size_t __real_strftime(char *, size_t, const char *, const struct tm *);
extern int __real_mysql_query(MYSQL *, const char *);
extern int __real_mysql_ping(MYSQL *);
extern int __real_pipe(int [2]);
extern int __real_socketpair(int, int, int, int [2]);
extern pid_t __real_fork(void);
extern int __real_dup2(int, int);
extern int __real_execve(const char *, char *const [], char *const []);
extern void *__real_malloc(size_t);
extern char *__real_strdup(const char *);
extern void *__real_snmp_sess_open(netsnmp_session *);
extern int __real_snmp_sess_close(void *);

static int format_failures;
static int format_fault_calls;
static MYSQL *query_fault_connection;
static const char *query_fault_statement;
static int query_fault_armed;
static int observed_query_error;
static int query_fault_calls;
static int query_ping_calls;
static int pipe_failures;
static int socketpair_failures;
static int fork_failures;
static int fork_failure_errno;
static int fork_fault_calls;
static int duplicate_failure_at;
static int duplicate_calls;
static int execute_failures;
static int allocation_failures;
static int copy_failures;
static bool account_snmp_sessions;
static void *owned_snmp_session;
static int session_opens;
static int session_close_attempts;
static int session_closes;

void *__wrap_snmp_sess_open(netsnmp_session *session) {
	void *handle = __real_snmp_sess_open(session);
	if (account_snmp_sessions && handle != NULL) {
		assert(owned_snmp_session == NULL);
		owned_snmp_session = handle;
		session_opens++;
	}
	return handle;
}

int __wrap_snmp_sess_close(void *handle) {
	bool owned = account_snmp_sessions && handle == owned_snmp_session && handle != NULL;
	if (owned) session_close_attempts++;
	int result = __real_snmp_sess_close(handle);
	if (owned && result == 1) {
		session_closes++;
		owned_snmp_session = NULL;
	}
	return result;
}

size_t __wrap_strftime(char *output, size_t capacity, const char *format, const struct tm *time) {
	if (format_failures > 0) {
		format_failures--;
		format_fault_calls++;
		return 0; /* strftime documents zero when its destination is insufficient. */
	}
	return __real_strftime(output, capacity, format, time);
}

int __wrap_mysql_query(MYSQL *mysql, const char *query) {
	if (query_fault_armed && mysql == query_fault_connection && strcmp(query, query_fault_statement) == 0) {
		query_fault_armed = 0;
		bool reconnect = FALSE;
		assert(mysql_options(mysql, MYSQL_OPT_RECONNECT, &reconnect) == 0);
		int result = __real_mysql_query(mysql, query);
		observed_query_error = (int)mysql_errno(mysql);
		assert(result != 0 && (observed_query_error == 2006 || observed_query_error == 2013));
		reconnect = TRUE;
		assert(mysql_options(mysql, MYSQL_OPT_RECONNECT, &reconnect) == 0);
		assert((int)mysql_errno(mysql) == observed_query_error);
		query_fault_calls++;
		return result;
	}
	return __real_mysql_query(mysql, query);
}

int __wrap_mysql_ping(MYSQL *mysql) {
	if (mysql == query_fault_connection) query_ping_calls++;
	return __real_mysql_ping(mysql);
}

int __wrap_pipe(int descriptors[2]) {
	if (pipe_failures > 0) { pipe_failures--; errno = EMFILE; return -1; }
	return __real_pipe(descriptors);
}

int __wrap_socketpair(int domain, int type, int protocol, int descriptors[2]) {
	if (socketpair_failures > 0) { socketpair_failures--; errno = EMFILE; return -1; }
	return __real_socketpair(domain, type, protocol, descriptors);
}

pid_t __wrap_fork(void) {
	if (fork_failures > 0) { fork_failures--; fork_fault_calls++; errno = fork_failure_errno; return -1; }
	return __real_fork();
}

int __wrap_dup2(int source, int destination) {
	duplicate_calls++;
	if (duplicate_failure_at > 0 && duplicate_calls == duplicate_failure_at) { errno = EBADF; return -1; }
	return __real_dup2(source, destination);
}

int __wrap_execve(const char *path, char *const arguments[], char *const environment[]) {
	if (execute_failures > 0) { execute_failures--; errno = ENOENT; return -1; }
	return __real_execve(path, arguments, environment);
}

void *__wrap_malloc(size_t size) {
	if (allocation_failures > 0) { allocation_failures--; errno = ENOMEM; return NULL; }
	return __real_malloc(size);
}

char *__wrap_strdup(const char *text) {
	if (copy_failures > 0) { copy_failures--; errno = ENOMEM; return NULL; }
	return __real_strdup(text);
}

static void test_logger_format_failure(void) {
	config_t previous = set;
	char filename[] = "spine-log-fault-XXXXXX";
	int log = mkstemp(filename);
	assert(log >= 0 && close(log) == 0);
	set.logging.log_destination = LOGDEST_FILE;
	set.logging.log_level = POLLER_VERBOSITY_LOW;
	set.logging.logfile_processed = TRUE;
	set.console.stdout_notty = TRUE;
	set.console.stderr_notty = FALSE;
	STRNCOPY(set.logging.path_logfile, filename);
	#ifdef DISABLE_STDERR
	const int diagnostic = STDOUT_FILENO;
	set.console.stdout_notty = FALSE;
	#else
	const int diagnostic = STDERR_FILENO;
	#endif
	int saved = dup(diagnostic);
	int capture[2];
	assert(saved >= 0 && pipe(capture) == 0);
	fflush(NULL);
	assert(dup2(capture[1], diagnostic) == diagnostic && close(capture[1]) == 0);
	format_failures = 1;
	format_fault_calls = 0;
	/* Date formatting failure reports a warning and preserves sink delivery. */
	assert(spine_log("regression date fallback") == TRUE);
	assert(format_failures == 0 && format_fault_calls == 1);
	fflush(NULL);
	assert(dup2(saved, diagnostic) == diagnostic && close(saved) == 0);
	char response[LOGSIZE * 2];
	ssize_t received = read(capture[0], response, sizeof(response) - 1);
	assert(received > 0 && (size_t)received < sizeof(response) && close(capture[0]) == 0);
	response[received] = '\0';
	assert(strstr(response, "ERROR: Could not get string from strftime()") != NULL);
	assert(strstr(response, "regression date fallback") != NULL);

	set.console.stdout_notty = TRUE;
	set.console.stderr_notty = TRUE;
	saved = dup(diagnostic);
	assert(saved >= 0 && pipe(capture) == 0);
	assert(dup2(capture[1], diagnostic) == diagnostic && close(capture[1]) == 0);
	format_failures = 1;
	assert(spine_log("regression quiet date fallback") == TRUE);
	fflush(NULL);
	assert(dup2(saved, diagnostic) == diagnostic && close(saved) == 0);
	assert(read(capture[0], response, sizeof(response)) == 0 && close(capture[0]) == 0);
	assert(format_failures == 0 && format_fault_calls == 2);
	FILE *file = fopen(filename, "r");
	assert(file != NULL);
	size_t bytes = fread(response, 1, sizeof(response) - 1, file);
	assert(bytes > 0 && bytes < sizeof(response) && !ferror(file) && fclose(file) == 0);
	response[bytes] = '\0';
	assert(strstr(response, "regression date fallback") != NULL);
	assert(strstr(response, "regression quiet date fallback") != NULL);
	assert(strncmp(response, "SPINE: Poller[", strlen("SPINE: Poller[")) == 0);
	assert(unlink(filename) == 0);
	set = previous;
	puts("production logger date-format failure regressions passed");
}

static unsigned int open_descriptor_count(void) {
	DIR *directory = opendir("/proc/self/fd");
	assert(directory != NULL);
	unsigned int count = 0;
	const struct dirent *entry;
	while ((entry = readdir(directory)) != NULL) {
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) count++;
	}
	assert(closedir(directory) == 0);
	return count;
}

static void assert_cancel_state(int expected) {
	int previous;
	assert(pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous) == 0);
	assert(previous == expected);
	assert(pthread_setcancelstate(previous, NULL) == 0);
}

static void test_process_creation_failures(void) {
	unsigned int descriptors = open_descriptor_count();
	int unrelated[2];
	assert(pipe(unrelated) == 0);
	unsigned int with_unrelated = open_descriptor_count();
	assert(with_unrelated == descriptors + 2);
	pipe_failures = 1;
	assert(nft_popen("printf unused", "r") == -1 && errno == EMFILE);
	assert(pipe_failures == 0 && open_descriptor_count() == with_unrelated);
	socketpair_failures = 1;
	assert(nft_popen("printf unused", "r+") == -1 && errno == EMFILE);
	assert(socketpair_failures == 0 && open_descriptor_count() == with_unrelated);
	allocation_failures = 1;
	assert(nft_popen("printf unused", "r") == -1 && errno == ENOMEM);
	assert(allocation_failures == 0 && open_descriptor_count() == with_unrelated);
	assert_cancel_state(PTHREAD_CANCEL_ENABLE);
	copy_failures = 1;
	assert(nft_popen("printf unused", "r") == -1 && errno == ENOMEM);
	assert(copy_failures == 0 && open_descriptor_count() == with_unrelated);
	assert_cancel_state(PTHREAD_CANCEL_ENABLE);
	fork_failures = 4;
	fork_failure_errno = EAGAIN;
	fork_fault_calls = 0;
	assert(nft_popen("printf unused", "r") == -1 && errno == EAGAIN);
	assert(fork_failures == 0 && fork_fault_calls == 4 && open_descriptor_count() == with_unrelated);
	assert_cancel_state(PTHREAD_CANCEL_ENABLE);
	fork_failures = 1;
	fork_failure_errno = EPERM;
	fork_fault_calls = 0;
	assert(nft_popen("printf unused", "r") == -1 && errno == EPERM);
	assert(fork_failures == 0 && fork_fault_calls == 1 && open_descriptor_count() == with_unrelated);
	fork_failures = 2;
	fork_failure_errno = EAGAIN;
	fork_fault_calls = 0;
	int descriptor = nft_popen("printf ok", "r");
	assert(descriptor >= 0 && fork_failures == 0 && fork_fault_calls == 2);
	char bytes[3] = {0};
	assert(read(descriptor, bytes, 2) == 2 && strcmp(bytes, "ok") == 0);
	int status = nft_pclose(descriptor);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	assert(open_descriptor_count() == with_unrelated);
	for (int index = 0; index < 3; index++) {
		duplicate_calls = 0;
		duplicate_failure_at = index < 2 ? index + 1 : 0;
		execute_failures = index == 2 ? 1 : 0;
		descriptor = nft_popen("printf forbidden", index == 1 ? "r+" : "r");
		assert(descriptor >= 0);
		assert(read(descriptor, bytes, sizeof(bytes)) == 0);
		status = nft_pclose(descriptor);
		assert(WIFEXITED(status) && WEXITSTATUS(status) == 127);
		assert(open_descriptor_count() == with_unrelated);
		duplicate_failure_at = 0;
		execute_failures = 0;
	}
	assert(write(unrelated[1], "x", 1) == 1 && read(unrelated[0], bytes, 1) == 1 && bytes[0] == 'x');
	assert(close(unrelated[0]) == 0 && close(unrelated[1]) == 0);
	assert(open_descriptor_count() == descriptors);
	puts("production process creation failure regressions passed");
}

static void kill_owned_connection(MYSQL *administrator, MYSQL *victim) {
	char query[100];
	spine_snprintf(query, sizeof(query), "KILL CONNECTION %lu", mysql_thread_id(victim));
	assert(mysql_query(administrator, query) == 0);
}

static void arm_lost_connection(MYSQL *mysql, const char *statement) {
	query_fault_connection = mysql;
	query_fault_statement = statement;
	query_fault_armed = 1;
	query_fault_calls = 0;
	query_ping_calls = 0;
	observed_query_error = 0;
}

static void test_real_database_retry(void) {
	const char *hostname = getenv("SPINE_TEST_DB_HOST");
	assert(hostname != NULL && hostname[0] != '\0');
	config_defaults();
	strncopy(set.database.host, hostname, sizeof(set.database.host));
	STRNCOPY(set.database.user, "root");
	STRNCOPY(set.database.database, "spine_regressions");
	set.database.password[0] = '\0';
	set.database.port = 3306;
	set.poller.SQL_readonly = FALSE;
	set.logging.log_destination = 0;
	set.console.stdout_notty = TRUE;
	set.console.stderr_notty = TRUE;
	MYSQL administrator;
	MYSQL victim;
	db_connect(LOCAL, &administrator);
	db_connect(LOCAL, &victim);
	unsigned long original = mysql_thread_id(&victim);
	kill_owned_connection(&administrator, &victim);
	const char *read_statement = "SELECT 123 AS recovered_value";
	arm_lost_connection(&victim, read_statement);
	MYSQL_RES *result = db_query(&victim, LOCAL, read_statement);
	assert(result != NULL && mysql_num_rows(result) == 1);
	MYSQL_ROW row = mysql_fetch_row(result);
	assert(row != NULL && strcmp(row[0], "123") == 0);
	db_free_result(result);
	assert(query_fault_calls == 1 && query_ping_calls == 1 && !query_fault_armed && observed_query_error != 0);
	assert(mysql_thread_id(&victim) != original);
	char table[100];
	spine_snprintf(table, sizeof(table), "spine_fault_%ld", (long)getpid());
	char query[BUFSIZE];
	spine_snprintf(query, sizeof(query), "CREATE TABLE %s (value INT NOT NULL) ENGINE=InnoDB", table);
	assert(db_insert(&administrator, LOCAL, query));
	original = mysql_thread_id(&victim);
	kill_owned_connection(&administrator, &victim);
	spine_snprintf(query, sizeof(query), "INSERT INTO %s(value) VALUES(456)", table);
	arm_lost_connection(&victim, query);
	assert(db_insert(&victim, LOCAL, query));
	assert(query_fault_calls == 1 && query_ping_calls == 1 && !query_fault_armed && observed_query_error != 0);
	assert(mysql_thread_id(&victim) != original);
	spine_snprintf(query, sizeof(query), "SELECT value FROM %s", table);
	result = db_query(&administrator, LOCAL, query);
	assert(result != NULL && mysql_num_rows(result) == 1);
	row = mysql_fetch_row(result);
	assert(row != NULL && strcmp(row[0], "456") == 0);
	db_free_result(result);
	spine_snprintf(query, sizeof(query), "DROP TABLE %s", table);
	assert(db_insert(&administrator, LOCAL, query));
	db_disconnect(&victim);
	db_disconnect(&administrator);
	query_fault_connection = NULL;
	query_fault_statement = NULL;
	puts("production real lost-connection retry regressions passed");
}

static unsigned long long fault_database_count(MYSQL *mysql, const char *query) {
	MYSQL_RES *result = db_query(mysql, LOCAL, query);
	assert(result != NULL && mysql_num_rows(result) == 1);
	MYSQL_ROW row = mysql_fetch_row(result);
	assert(row != NULL && row[0] != NULL);
	unsigned long long count = strtoull(row[0], NULL, 10);
	db_free_result(result);
	return count;
}

static void *run_ping_only_worker(void *argument) {
	assert(mysql_thread_init() == 0);
	int errors = -1;
	poll_host(argument, &errors);
	assert(errors == -1); /* Early ping-only return preserves this output. */
	return NULL;
}

static void test_ping_only_session_lifetime(void) {
	extern int *debug_devices;
	const char *agent = getenv("SPINE_TEST_SNMP_HOST");
	assert(agent != NULL && agent[0] != '\0');
	config_t previous = set;
	int *previous_debug_devices = debug_devices;
	int debug_fixture[100] = {0};
	debug_devices = debug_fixture;
	pool_t *previous_pool = db_pool_local;
	set.poller.threads = 1;
	set.poller.poller_id = 1;
	set.poller.mode = REMOTE_OFFLINE;
	set.availability.ping_only = TRUE;
	set.snmp.snmp_retries = 0;
	set.snmp.mibs = 0;
	MYSQL administrator;
	db_connect(LOCAL, &administrator);
	/* Refuse to replace pre-existing records, even in the isolated fixture. */
	assert(fault_database_count(&administrator, "SELECT COUNT(*) FROM host WHERE id=900") == 0);
	assert(fault_database_count(&administrator, "SELECT COUNT(*) FROM poller_item WHERE host_id=900") == 0);
	unsigned long long outputs = fault_database_count(&administrator, "SELECT COUNT(*) FROM poller_output");
	unsigned long long boosted = fault_database_count(&administrator, "SELECT COUNT(*) FROM poller_output_boost");
	db_pool_local = calloc(1, sizeof(*db_pool_local));
	assert(db_pool_local != NULL);
	db_pool_local[0].free = TRUE;
	db_connect(LOCAL, &db_pool_local[0].mysql);
	assert(spine_permits_init(&available_threads, 2) == 0);
	assert(spine_permits_init(&available_scripts, 3) == 0);
	char escaped_agent[BUFSIZE];
	db_escape(&administrator, escaped_agent, sizeof(escaped_agent), agent);
	snmp_spine_init();
	for (int with_session = 1; with_session >= 0; with_session--) {
		char query[LRG_BUFSIZE];
		spine_snprintf(query, sizeof(query), "INSERT INTO host(id,hostname,snmp_version,snmp_community,snmp_port,snmp_timeout,availability_method,status,total_polls,failed_polls,status_fail_date,status_rec_date,snmp_sysLocation) VALUES(900,'%s',2,'%s',1161,500,%i,%i,0,0,'2026-10-06 00:00:00','2026-10-06 00:00:00','ping-only-metadata')", escaped_agent, with_session ? "regression" : "", AVAIL_SNMP, HOST_UP);
		assert(db_insert(&administrator, LOCAL, query));
		poller_thread_t work = {0};
		work.host_id = 900;
		work.host_thread = 1;
		work.host_threads = 1;
		work.host_time_double = get_time_as_double();
		STRNCOPY(work.host_time, "1791244800");
		owned_snmp_session = NULL;
		session_opens = 0;
		session_close_attempts = 0;
		session_closes = 0;
		account_snmp_sessions = TRUE;
		pthread_t worker;
		assert(pthread_create(&worker, NULL, run_ping_only_worker, &work) == 0);
		assert(pthread_join(worker, NULL) == 0);
		account_snmp_sessions = FALSE;
		int observed_opens = session_opens;
		int observed_attempts = session_close_attempts;
		int observed_closes = session_closes;
		/* Preserve an old-code failure without leaking the task-owned fixture
		 * handle: this real cleanup is excluded from production close counts. */
		if (owned_snmp_session != NULL) {
			assert(__real_snmp_sess_close(owned_snmp_session) == 1);
			owned_snmp_session = NULL;
		}
		assert(db_pool_local[0].free);
		assert(spine_permits_available(&available_threads) == 2);
		assert(spine_permits_available(&available_scripts) == 3);
		spine_snprintf(query, sizeof(query), "SELECT COUNT(*) FROM host WHERE id=900 AND status=%i AND total_polls=1 AND failed_polls=0 AND snmp_sysLocation='ping-only-metadata'", HOST_UP);
		assert(fault_database_count(&administrator, query) == 1);
		assert(fault_database_count(&administrator, "SELECT COUNT(*) FROM poller_output") == outputs);
		assert(fault_database_count(&administrator, "SELECT COUNT(*) FROM poller_output_boost") == boosted);
		assert(db_insert(&administrator, LOCAL, "DELETE FROM host WHERE id=900"));
		fprintf(stderr, "ping-only owned session accounting: session=%d opens=%d attempts=%d closes=%d\n", with_session, observed_opens, observed_attempts, observed_closes);
		assert(observed_opens == with_session);
		assert(observed_attempts == with_session && observed_closes == with_session);
	}
	snmp_spine_close();
	assert(spine_permits_destroy(&available_scripts) == 0);
	assert(spine_permits_destroy(&available_threads) == 0);
	db_close_connection_pool(LOCAL);
	db_pool_local = previous_pool;
	db_disconnect(&administrator);
	debug_devices = previous_debug_devices;
	set = previous;
	puts("production ping-only SNMP session ownership regressions passed");
}

int main(int argc, char **argv) {
	config_defaults();
	init_mutexes();
	alarm(20);
	test_logger_format_failure();
	test_process_creation_failures();
	if (argc == 2 && strcmp(argv[1], "--database") == 0) {
		test_real_database_retry();
		test_ping_only_session_lifetime();
	}
	else assert(argc == 1);
	alarm(0);
	puts("production linker fault regressions passed");
	return 0;
}
