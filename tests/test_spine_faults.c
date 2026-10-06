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
	set.log_destination = LOGDEST_FILE;
	set.log_level = POLLER_VERBOSITY_LOW;
	set.logfile_processed = TRUE;
	set.stdout_notty = TRUE;
	set.stderr_notty = FALSE;
	STRNCOPY(set.path_logfile, filename);
	#ifdef DISABLE_STDERR
	const int diagnostic = STDOUT_FILENO;
	set.stdout_notty = FALSE;
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
	assert(received > 0 && close(capture[0]) == 0);
	response[received] = '\0';
	assert(strstr(response, "ERROR: Could not get string from strftime()") != NULL);
	assert(strstr(response, "regression date fallback") != NULL);

	set.stdout_notty = TRUE;
	set.stderr_notty = TRUE;
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
	assert(bytes > 0 && !ferror(file) && fclose(file) == 0);
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
	struct dirent *entry;
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
	strncopy(set.db_host, hostname, sizeof(set.db_host));
	STRNCOPY(set.db_user, "root");
	STRNCOPY(set.db_db, "spine_regressions");
	set.db_pass[0] = '\0';
	set.db_port = 3306;
	set.SQL_readonly = FALSE;
	set.log_destination = 0;
	set.stdout_notty = TRUE;
	set.stderr_notty = TRUE;
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

int main(int argc, char **argv) {
	config_defaults();
	init_mutexes();
	alarm(20);
	test_logger_format_failure();
	test_process_creation_failures();
	if (argc == 2 && strcmp(argv[1], "--database") == 0) test_real_database_retry();
	else assert(argc == 1);
	alarm(0);
	puts("production linker fault regressions passed");
	return 0;
}
