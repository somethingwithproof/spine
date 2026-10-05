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
#include <sys/un.h>
#include <limits.h>

#ifdef main
#undef main
#endif

static void test_copy_bounds(void) {
	struct { char text[8]; unsigned char guard; } output;
	memset(&output, 0xa5, sizeof output);
	strncopy(output.text, "12345678", sizeof(output.text));
	assert(strcmp(output.text, "1234567") == 0);
	assert(output.guard == 0xa5);
	strncopy(output.text, "abcdefghijk", sizeof(output.text));
	assert(strcmp(output.text, "abcdefg") == 0);
	assert(output.guard == 0xa5);
	strncopy(output.text, "unchanged", 0);
	assert(strcmp(output.text, "abcdefg") == 0);
	strncopy(output.text, "x", 1);
	assert(output.text[0] == '\0');
	strncopy(output.text, "", sizeof(output.text));
	assert(output.text[0] == '\0');
}

static void test_string_conversions(void) {
	char empty[] = "";
	char one[] = "x";
	char word[] = "abcd";
	assert(strcmp(reverse(empty), "") == 0);
	assert(strcmp(reverse(one), "x") == 0);
	assert(strcmp(reverse(word), "dcba") == 0);
	char hex[] = "\"FF ff\t\"";
	char maximum[] = "FFFFFFFFFFFFFFFF";
	char overflow[] = "10000000000000000";
	char invalid[] = "not hex";
	assert(hex2dec(hex) == 65535);
	assert(hex2dec(maximum) == ULLONG_MAX);
	assert(hex2dec(overflow) == 0);
	assert(hex2dec(invalid) == 0 && hex2dec(NULL) == 0);
	char input[BUFSIZE * 2];
	memset(input, '\\', sizeof(input) - 1);
	input[sizeof(input) - 1] = '\0';
	char *escaped = add_slashes(input);
	assert(strlen(escaped) == 2 * (sizeof(input) - 1));
	for (size_t i = 0; escaped[i] != '\0'; i++) assert(escaped[i] == '\\');
	free(escaped);
	escaped = add_slashes("a\\b");
	assert(strcmp(escaped, "a\\\\b") == 0);
	free(escaped);
	escaped = add_slashes("");
	assert(escaped[0] == '\0');
	free(escaped);
	char decorated[] = "text +12.5 Bytes";
	assert(strcmp(strip_alpha(decorated), "12.5") == 0);
	char negative[] = "text -12.5 Bytes";
	assert(strcmp(strip_alpha(negative), "-12.5") == 0);
	char nonnumeric[] = "no sample";
	assert(strcmp(strip_alpha(nonnumeric), "") == 0);
}

static void test_result_count_range(void) {
	assert(spine_count_to_int(0) == 0);
	assert(spine_count_to_int(INT_MAX) == INT_MAX);
	pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		spine_count_to_int((unsigned long long)INT_MAX + 1);
		_exit(99);
	}
	int status;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_FAILURE);
}

static spine_permits_t test_permits;
static pthread_mutex_t permit_test_mutex = PTHREAD_MUTEX_INITIALIZER;
static int active_permit_workers;

static void *exercise_permit(void *unused) {
	(void)unused;
	for (int iteration = 0; iteration < 100; iteration++) {
		int status;
		while ((status = spine_permits_try_acquire(&test_permits)) == EAGAIN) spine_sleep_usec(100);
		assert(status == 0);
		assert(pthread_mutex_lock(&permit_test_mutex) == 0);
		assert(active_permit_workers == 0);
		active_permit_workers++;
		assert(pthread_mutex_unlock(&permit_test_mutex) == 0);
		spine_sleep_usec(100);
		assert(pthread_mutex_lock(&permit_test_mutex) == 0);
		assert(active_permit_workers == 1);
		active_permit_workers--;
		assert(pthread_mutex_unlock(&permit_test_mutex) == 0);
		assert(spine_permits_release(&test_permits) == 0);
	}
	return NULL;
}

static void test_concurrent_permits(void) {
	assert(spine_permits_init(&test_permits, -1) == EINVAL);
	assert(spine_permits_init(&test_permits, INT_MAX) == 0);
	assert(spine_permits_release(&test_permits) == EOVERFLOW);
	assert(spine_permits_available(&test_permits) == INT_MAX);
	assert(spine_permits_destroy(&test_permits) == 0);
	assert(spine_permits_init(&test_permits, 1) == 0);
	assert(spine_permits_try_acquire(&test_permits) == 0);
	assert(spine_permits_try_acquire(&test_permits) == EAGAIN);
	assert(spine_permits_available(&test_permits) == 0);
	assert(spine_permits_release(&test_permits) == 0);
	pthread_t workers[4];
	for (size_t i = 0; i < sizeof(workers) / sizeof(workers[0]); i++) {
		assert(pthread_create(&workers[i], NULL, exercise_permit, NULL) == 0);
	}
	for (size_t i = 0; i < sizeof(workers) / sizeof(workers[0]); i++) assert(pthread_join(workers[i], NULL) == 0);
	assert(spine_permits_available(&test_permits) == 1 && active_permit_workers == 0);
	assert(spine_permits_destroy(&test_permits) == 0);
}

static void test_database_escape(void) {
	MYSQL *mysql = mysql_init(NULL);
	assert(mysql != NULL);
	char input[RESULTS_BUFFER];
	char output[RESULTS_BUFFER * 2 + 1];
	memset(input, 'x', sizeof(input) - 1);
	input[sizeof(input) - 1] = '\0';
	db_escape(mysql, output, sizeof(output), input);
	assert(strcmp(output, input) == 0);
	memset(input, '\'', sizeof(input) - 1);
	db_escape(mysql, output, sizeof(output), input);
	assert(strlen(output) == 2 * (sizeof(input) - 1));
	for (size_t i = 0; i < sizeof(input) - 1; i++) {
		assert(output[2 * i] == '\\' && output[2 * i + 1] == '\'');
	}
	unsigned char guarded[13];
	memset(guarded, 0xa5, sizeof(guarded));
	db_escape(mysql, (char *)guarded + 1, 11, input);
	assert(guarded[0] == 0xa5 && guarded[12] == 0xa5);
	assert(strlen((char *)guarded + 1) == 10);
	char tiny[] = "abc";
	db_escape(mysql, tiny, 0, input);
	assert(strcmp(tiny, "abc") == 0);
	db_escape(mysql, tiny, -1, input);
	assert(strcmp(tiny, "abc") == 0);
	db_escape(mysql, tiny, 1, input);
	assert(tiny[0] == '\0' && tiny[1] == 'b');
	db_escape(mysql, tiny, sizeof(tiny), NULL);
	assert(tiny[1] == 'b');
	db_escape(mysql, NULL, 16, input);
	mysql_close(mysql);
}

static void test_database_addresses(void) {
	db_address_t address;
	db_address_init(&address, "localhost", true);
	assert(strcmp(address.hostname, "localhost") == 0 && address.socket == NULL);
	db_address_release(&address);
	assert(address.storage == NULL && address.hostname == NULL && address.socket == NULL);
	db_address_init(&address, "localhost:service", true);
	assert(strcmp(address.hostname, "localhost") == 0 && strcmp(address.socket, "service") == 0);
	db_address_release(&address);
	db_address_init(&address, "remote:service", false);
	assert(strcmp(address.hostname, "remote:service") == 0 && address.socket == NULL);
	db_address_release(&address);
	char directory[] = "spine-socket-test-XXXXXX";
	assert(mkdtemp(directory) != NULL);
	db_address_init(&address, directory, true);
	assert(strcmp(address.hostname, directory) == 0 && address.socket == NULL);
	db_address_release(&address);
	struct sockaddr_un socket_address = {0};
	socket_address.sun_family = AF_UNIX;
	#ifdef __APPLE__
	socket_address.sun_len = sizeof(socket_address);
	#endif
	spine_snprintf(socket_address.sun_path, sizeof(socket_address.sun_path), "%s/db.sock", directory);
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	assert(fd >= 0);
	int bound = bind(fd, (struct sockaddr *)&socket_address, sizeof(socket_address));
	if (bound != 0) perror("bind regression socket");
	assert(bound == 0);
	db_address_init(&address, socket_address.sun_path, true);
	assert(address.hostname == NULL && strcmp(address.socket, socket_address.sun_path) == 0);
	db_address_release(&address);
	assert(close(fd) == 0);
	assert(unlink(socket_address.sun_path) == 0);
	assert(rmdir(directory) == 0);
}

static void test_database_option_failure(void) {
	MYSQL *mysql = mysql_init(NULL);
	assert(mysql != NULL);
	unsigned int timeout = 5;
	db_set_option(mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout, "test timeout");
	pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		db_set_option(mysql, (enum mysql_option)999999, NULL, "unsupported test option");
		_exit(0);
	}
	int status;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_FAILURE);
	mysql_close(mysql);
}

static void test_regex_storage(void) {
	char first[] = "abc123xyz";
	char second[] = "x4y";
	char absent[] = "no digits";
	const char *match = regex_replace("[0-9][0-9]*", first);
	assert(strcmp(match, "123") == 0);
	/* The result remains valid across another function's stack use. */
	char stack[4096];
	memset(stack, 'z', sizeof stack);
	assert(strcmp(match, "123") == 0);
	assert(strcmp(regex_replace("[0-9][0-9]*", second), "4") == 0);
	assert(regex_replace("[0-9][0-9]*", absent) == absent);
	assert(regex_replace("[", first) == first);
	char large[RESULTS_BUFFER + 1];
	memset(large, '9', sizeof large - 1);
	large[sizeof large - 1] = '\0';
	assert(regex_replace("[0-9][0-9]*", large) == large);
}

static pthread_mutex_t regex_test_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t regex_test_ready = PTHREAD_COND_INITIALIZER;
static int regex_threads_ready;

static void *test_regex_thread(void *argument) {
	char *input = argument;
	const char *match = regex_replace("[0-9][0-9]*", input);
	assert(pthread_mutex_lock(&regex_test_mutex) == 0);
	regex_threads_ready++;
	assert(pthread_cond_broadcast(&regex_test_ready) == 0);
	while (regex_threads_ready < 2) {
		assert(pthread_cond_wait(&regex_test_ready, &regex_test_mutex) == 0);
	}
	/* Both threads have produced a result; neither may overwrite the other. */
	assert(strcmp(match, input) == 0);
	assert(pthread_mutex_unlock(&regex_test_mutex) == 0);
	return NULL;
}

static void test_regex_concurrency(void) {
	char first[] = "123";
	char second[] = "4567";
	pthread_t threads[2];
	assert(pthread_create(&threads[0], NULL, test_regex_thread, first) == 0);
	assert(pthread_create(&threads[1], NULL, test_regex_thread, second) == 0);
	assert(pthread_join(threads[0], NULL) == 0);
	assert(pthread_join(threads[1], NULL) == 0);
}

static void test_formatting(void) {
	char output[8];
	assert(spine_snprintf(output, sizeof output, "%s", "1234567") == 7);
	assert(strcmp(output, "1234567") == 0);
	pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		spine_snprintf(output, sizeof output, "%s", "12345678");
		_exit(0);
	}
	int status;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_FAILURE);
}

static void test_secret_clear(void) {
	unsigned char bytes[8];
	memset(bytes, 0xa5, sizeof bytes);
	spine_clear_sensitive(bytes + 1, 6);
	assert(bytes[0] == 0xa5 && bytes[7] == 0xa5);
	for (size_t i = 1; i < 7; i++) assert(bytes[i] == 0);
	spine_clear_sensitive(NULL, 0);
}

static void test_log_boundary(void) {
	char *message = malloc(LOGSIZE + 1);
	assert(message != NULL);
	memset(message, 'x', LOGSIZE);
	message[LOGSIZE] = '\0';
	set.log_level = POLLER_VERBOSITY_NONE;
	set.log_destination = 0;
	set.stdout_notty = TRUE;
	spine_log("%s", message);
	free(message);
}

static void test_log_sanitization(void) {
	char message[] = "a\r\nb\t\033\177c\302\205d\342\200\250e\342\200\251f";
	spine_sanitize_log_message(message);
	assert(strcmp(message, "a  b   c  d   e   f") == 0);
	char unicode[] = "caf\303\251";
	spine_sanitize_log_message(unicode);
	assert(strcmp(unicode, "caf\303\251") == 0);
	char truncated[] = "\342\200";
	spine_sanitize_log_message(truncated);
	assert(strcmp(truncated, "\342\200") == 0);
	char path[] = "spine-log-test-XXXXXX";
	int fd = mkstemp(path);
	assert(fd >= 0 && close(fd) == 0);
	set.log_destination = LOGDEST_FILE;
	set.log_level = POLLER_VERBOSITY_LOW;
	set.logfile_processed = TRUE;
	strncopy(set.path_logfile, path, sizeof(set.path_logfile));
	spine_log("device: %s", "first\r\nsecond");
	FILE *file = fopen(path, "r");
	assert(file != NULL);
	char line[BUFSIZE];
	assert(fgets(line, sizeof(line), file) != NULL);
	assert(strstr(line, "device: first  second") != NULL);
	assert(fgets(line, sizeof(line), file) == NULL && !ferror(file));
	assert(fclose(file) == 0 && unlink(path) == 0);
	set.log_destination = 0;
}

static void test_config_directives(void) {
	char path[] = "spine-config-test-XXXXXX";
	int fd = mkstemp(path);
	assert(fd >= 0);
	FILE *file = fdopen(fd, "w");
	assert(file != NULL);
	fputs("# comment\nDB_Host localhost\nDB_Port 3307\nRDB_UseSSL 1\n"
	      "Poller 2\nCacti_Log /tmp/spine-test.log\n"
	      "SNMP_Clientaddr 127.0.0.1\nDB_Pass\nDB_Database final_line", file);
	assert(fclose(file) == 0);
	memset(&set, 0, sizeof set);
	set.stdout_notty = TRUE;
	set.stderr_notty = TRUE;
	config_defaults();
	assert(read_spine_config(path) == 0);
	assert(strcmp(set.db_host, "localhost") == 0);
	assert(set.db_port == 3307 && set.rdb_ssl == 1 && set.poller_id == 2);
	assert(strcmp(set.path_logfile, "/tmp/spine-test.log") == 0);
	assert(strcmp(set.snmp_clientaddr, "127.0.0.1") == 0);
	assert(strcmp(set.db_db, "final_line") == 0);
	assert(set.logfile_processed == 1 && set.log_destination == LOGDEST_BOTH);
	assert(unlink(path) == 0);
	assert(read_spine_config(path) == -1);
}

static void test_date_formats(void) {
	const char *const expected[] = {
		"%m/%d/%Y %H:%M:%S - ", "%b/%d/%Y %H:%M:%S - ",
		"%d/%m/%Y %H:%M:%S - ", "%d/%b/%Y %H:%M:%S - ",
		"%Y/%m/%d %H:%M:%S - ", "%Y/%b/%d %H:%M:%S - "
	};
	set.log_datetime_separator = GDC_SLASH;
	for (int i = GD_MIN; i <= GD_MAX; i++) {
		set.log_datetime_format = i;
		char *format = get_date_format();
		assert(strcmp(format, expected[i]) == 0);
		free(format);
	}
	set.log_datetime_format = -1;
	set.log_datetime_separator = -1;
	char *format = get_date_format();
	assert(strcmp(format, expected[GD_DEFAULT]) == 0);
	free(format);
}

static void test_device_logging(void) {
	extern int *debug_devices;
	int devices[100] = {7, 0};
	int *previous = debug_devices;
	debug_devices = devices;
	for (int level = POLLER_VERBOSITY_NONE; level <= POLLER_VERBOSITY_DEVDBG; level++) {
		set.log_level = level;
		for (int minimum = POLLER_VERBOSITY_LOW; minimum <= POLLER_VERBOSITY_DEVDBG; minimum++) {
			assert(spine_should_log_device(7, minimum));
			assert(spine_should_log_device(8, minimum) == (level >= minimum));
		}
	}
	set.log_level = POLLER_VERBOSITY_NONE;
	int evaluated = 0;
	SPINE_LOG_DEVICE(8, POLLER_VERBOSITY_DEBUG, ("hidden %i", ++evaluated));
	assert(evaluated == 0);
	SPINE_LOG_DEVICE(7, POLLER_VERBOSITY_DEBUG, ("visible %i", ++evaluated));
	assert(evaluated == 1);
	debug_devices = previous;
}

static void test_debug_device_bounds(void) {
	struct { int before; int devices[100]; int after; } output;
	output.before = 123;
	output.after = 456;
	char list[2048];
	size_t used = 0;
	for (int i = 1; i <= 110; i++) {
		used += (size_t)spine_snprintf(list + used, sizeof(list) - used, "%i,", i);
	}
	parse_debug_devices(list, output.devices, 100);
	assert(output.before == 123 && output.after == 456);
	for (int i = 0; i < 99; i++) assert(output.devices[i] == i + 1);
	assert(output.devices[99] == 0);
	char empty[] = "";
	parse_debug_devices(empty, output.devices, 100);
	assert(output.devices[0] == 0);
	char single[] = "7";
	parse_debug_devices(single, output.devices, 1);
	assert(output.devices[0] == 0);
	output.devices[0] = 7;
	parse_debug_devices(single, output.devices, 0);
	assert(output.devices[0] == 7);
}

static void test_poll_result_formats(void) {
	const struct {
		const char *input;
		const char *output;
		enum poll_result_status status;
	} cases[] = {
		{"123", "123", POLL_RESULT_VALID},
		{"-12.5", "-12.5", POLL_RESULT_VALID},
		{"4096 Bytes", "4096", POLL_RESULT_VALID},
		{"a:1 b!2", "a:1 b!2", POLL_RESULT_VALID},
		{"a!1", "a!1", POLL_RESULT_VALID},
		{"U", "U", POLL_RESULT_UNDEFINED},
		{"nonsense", "", POLL_RESULT_INVALID}
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		for (int snmp = 0; snmp <= 1; snmp++) {
			char result[RESULTS_BUFFER];
			strncopy(result, cases[i].input, sizeof(result));
			assert(normalize_poll_result(result, snmp) == cases[i].status);
			assert(strcmp(result, cases[i].output) == 0);
		}
	}
	char undefined[RESULTS_BUFFER] = "u";
	assert(normalize_poll_result(undefined, true) == POLL_RESULT_UNDEFINED);
	assert(strcmp(undefined, "u") == 0);
	assert(normalize_poll_result(undefined, false) == POLL_RESULT_INVALID);
}

static void test_hostnames(void) {
	const char *const inputs[] = {"router.example", "router.example:161", "TCP:router.example:443",
		"udp:router.example:53", "[2001:db8::1]"};
	const char *const outputs[] = {"router.example", "router.example", "router.example", "router.example", "[2001:db8::1]"};
	const int ports[] = {0, 161, 443, 53, 0};
	const int methods[] = {0, 0, 1, 2, 0};
	for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
		char input[128];
		strncopy(input, inputs[i], sizeof(input));
		name_t *name = get_namebyhost(input, NULL);
		assert(strcmp(name->hostname, outputs[i]) == 0);
		assert(name->port == ports[i] && name->method == methods[i]);
		free(name);
	}
}

static void test_snmp_initialization_failure(void) {
	/* These inputs fail before a network session opens. Temporary connection
	 * parameters and passphrase copies must be released on every error path. */
	assert(snmp_host_init(1, "127.0.0.1", 99, "public", "", "", "SHA",
		"", "[None]", "", "", 161, 1000) == NULL);
	assert(snmp_host_init(1, "127.0.0.1", 3, "public", "user", "short", "INVALID",
		"", "[None]", "", "", 161, 1000) == NULL);
	assert(snmp_host_init(1, "127.0.0.1", 3, "public", "user", "short", "SHA",
		"short", "AES", "", "", 161, 1000) == NULL);
	assert(snmp_host_init(1, "127.0.0.1", 3, "public", "user", "regression-password", "SHA",
		"short", "AES", "", "", 161, 1000) == NULL);
}

static void test_child_process(void) {
	int fd = nft_popen("printf spine-test", "r");
	assert(fd >= 0);
	char output[32] = {0};
	size_t used = 0;
	ssize_t received;
	while ((received = read(fd, output + used, sizeof(output) - 1 - used)) != 0) {
		if (received < 0 && errno == EINTR) continue;
		assert(received > 0);
		used += (size_t)received;
		assert(used < sizeof(output) - 1);
	}
	assert(used == 10);
	assert(strcmp(output, "spine-test") == 0);
	int status = nft_pclose(fd);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	assert(nft_popen("printf ignored", "invalid") == -1);
}

static void test_php_response(size_t length, bool newline) {
	int pipes[2];
	assert(pipe(pipes) == 0);
	pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		close(pipes[0]);
		char *data = malloc(length);
		assert(data != NULL);
		memset(data, '7', length);
		if (newline) data[length - 1] = '\n';
		size_t sent = 0;
		while (sent < length) {
			ssize_t count = write(pipes[1], data + sent, length - sent);
			if (count < 0 && errno == EINTR) continue;
			assert(count > 0);
			sent += (size_t)count;
		}
		free(data);
		close(pipes[1]);
		_exit(0);
	}
	close(pipes[1]);
	char result[RESULTS_BUFFER];
	enum php_response_status response_status = php_read_response(pipes[0], result, sizeof(result), 5);
	if (newline) {
		assert(response_status == PHP_RESPONSE_OK);
		assert(strlen(result) == length && result[length - 1] == '\n');
	} else {
		assert(response_status == PHP_RESPONSE_TOO_LONG);
		assert(result[sizeof(result) - 1] == '\0');
	}
	close(pipes[0]);
	int status;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_php_partial_response_timeout(void) {
	int pipes[2];
	assert(pipe(pipes) == 0);
	assert(write(pipes[1], "7", 1) == 1);
	char result[16];
	double begin = spine_monotonic_time();
	assert(php_read_response(pipes[0], result, sizeof(result), 1) == PHP_RESPONSE_TIMEOUT);
	assert(spine_monotonic_time() - begin < 5);
	assert(strcmp(result, "7") == 0);
	close(pipes[1]);
	assert(php_read_response(pipes[0], result, sizeof(result), 1) == PHP_RESPONSE_EOF);
	assert(php_read_response(-1, result, sizeof(result), 1) == PHP_RESPONSE_ERROR);
	assert(php_read_response(FD_SETSIZE, result, sizeof(result), 1) == PHP_RESPONSE_ERROR);
	assert(php_read_response(pipes[0], result, 1, 1) == PHP_RESPONSE_ERROR);
	close(pipes[0]);
}

static void test_script_execution(void) {
	assert(spine_permits_init(&available_scripts, 1) == 0);
	host_t host = {0};
	STRNCOPY(host.hostname, "regression-device");
	int previous_timeout = set.script_timeout;
	set.script_timeout = 1;
	char command[] = "/usr/bin/printf 7";
	char *result = exec_poll(&host, command, 1, "DS");
	assert(strcmp(result, "7") == 0);
	free(result);
	char delayed[] = "/bin/sleep 2";
	double begin = spine_monotonic_time();
	result = exec_poll(&host, delayed, 1, "DS");
	assert(strcmp(result, "U") == 0);
	assert(spine_monotonic_time() - begin < 5);
	free(result);
	assert(spine_permits_destroy(&available_scripts) == 0);
	set.script_timeout = previous_timeout;
}

static void test_php_command(size_t length) {
	char command[BUFSIZE];
	assert(length <= sizeof(command) - 3);
	memset(command, 'c', length);
	command[length] = '\0';
	int requests[2];
	int responses[2];
	assert(pipe(requests) == 0 && pipe(responses) == 0);
	pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		close(requests[1]);
		close(responses[0]);
		char received[BUFSIZE] = {0};
		size_t used = 0;
		while (used < length + 2) {
			ssize_t bytes = read(requests[0], received + used, length + 2 - used);
			if (bytes < 0 && errno == EINTR) continue;
			assert(bytes > 0);
			used += (size_t)bytes;
		}
		assert(memcmp(received, command, length) == 0);
		assert(received[length] == '\r' && received[length + 1] == '\n');
		assert(write(responses[1], "7\n", 2) == 2);
		close(requests[0]);
		close(responses[1]);
		_exit(0);
	}
	close(requests[0]);
	close(responses[1]);
	php_t process = {0};
	process.php_write_fd = requests[1];
	process.php_read_fd = responses[0];
	php_t *previous = php_processes;
	php_processes = &process;
	set.php_servers = 1;
	set.script_timeout = 5;
	char *result = php_cmd(command, 0);
	assert(strcmp(result, "7\n") == 0);
	free(result);
	php_processes = previous;
	close(requests[1]);
	close(responses[0]);
	int status;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void test_invalid_php_commands(void) {
	/* Invalid requests must return before touching server state or writing. */
	char *result = php_cmd("invalid\r\nline", 0);
	assert(strcmp(result, "U") == 0);
	free(result);
	char oversized[BUFSIZE];
	memset(oversized, 'c', sizeof(oversized) - 1);
	oversized[sizeof(oversized) - 1] = '\0';
	result = php_cmd(oversized, 0);
	assert(strcmp(result, "U") == 0);
	free(result);
	result = php_cmd("valid", -1);
	assert(strcmp(result, "U") == 0);
	free(result);
}

static int run_test_script_server(int argc, char **argv) {
	assert(argc >= 5 && strcmp(argv[2], "regression-server") == 0);
	if (strcmp(argv[3], "spine") == 0) {
		assert(argc == 5 && strcmp(argv[4], "1") == 0);
	} else {
		assert(strcmp(argv[3], "--environ=spine") == 0);
		assert(strcmp(argv[4], "--poller=1") == 0 || strcmp(argv[4], "--poller=2") == 0);
		if (strcmp(argv[4], "--poller=2") == 0) {
			assert(argc == 6 && (strcmp(argv[5], "--mode=online") == 0 || strcmp(argv[5], "--mode=offline") == 0));
		} else assert(argc == 5);
	}
	puts("Started");
	fflush(stdout);
	char command[BUFSIZE];
	while (fgets(command, sizeof(command), stdin) != NULL) {
		if (strcmp(command, "quit\r\n") == 0) return 0;
		puts("7");
		fflush(stdout);
	}
	return 0;
}

static void test_php_startup(const char *executable) {
	char *absolute = realpath(executable, NULL);
	assert(absolute != NULL);
	config_t previous_config = set;
	php_t *previous_processes = php_processes;
	php_t processes[2];
	memset(processes, 0, sizeof(processes));
	php_processes = processes;
	STRNCOPY(set.path_php, absolute);
	free(absolute);
	STRNCOPY(set.path_php_server, "regression-server");
	set.script_timeout = 5;
	set.php_servers = 2;
	const int versions[] = {1222, 1223, 1223, 1223};
	const int pollers[] = {1, 1, 2, 2};
	const int modes[] = {REMOTE_ONLINE, REMOTE_ONLINE, REMOTE_ONLINE, REMOTE_OFFLINE};
	for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
		set.cacti_version = versions[i];
		set.poller_id = pollers[i];
		set.mode = modes[i];
		assert(php_init(PHP_INIT));
		for (int index = 0; index < set.php_servers; index++) {
			assert(processes[index].php_state == PHP_READY);
			char *result = php_cmd("regression request", index);
			assert(strcmp(result, "7\n") == 0);
			free(result);
		}
		pid_t children[] = {processes[0].php_pid, processes[1].php_pid};
		php_close(PHP_INIT);
		for (size_t index = 0; index < sizeof(children) / sizeof(children[0]); index++) {
			int status;
			assert(waitpid(children[index], &status, 0) == children[index]);
			assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
		}
	}
	assert(!php_init(-2) && !php_init(set.php_servers));
	STRNCOPY(set.path_php, "/nonexistent-spine-regression-executable");
	assert(!php_init(0));
	assert(processes[0].php_state == PHP_BUSY);
	assert(processes[0].php_read_fd == -1 && processes[0].php_write_fd == -1);
	int status;
	assert(waitpid(processes[0].php_pid, &status, 0) == processes[0].php_pid);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 127);
	php_processes = previous_processes;
	set = previous_config;
}

int main(int argc, char **argv) {
	if (argc > 1 && strcmp(argv[1], "-q") == 0) return run_test_script_server(argc, argv);
	extern int *debug_devices;
	static int devices[100];
	debug_devices = devices;
	test_string_conversions();
	test_result_count_range();
	test_concurrent_permits();
	test_copy_bounds();
	test_database_escape();
	test_database_addresses();
	test_database_option_failure();
	test_regex_concurrency();
	test_regex_storage();
	test_formatting();
	test_secret_clear();
	test_log_boundary();
	test_log_sanitization();
	test_config_directives();
	test_date_formats();
	test_device_logging();
	test_debug_device_bounds();
	test_poll_result_formats();
	test_hostnames();
	test_snmp_initialization_failure();
	test_child_process();
	test_php_response(4, true);
	test_php_response(RESULTS_BUFFER - 1, true);
	test_php_response(RESULTS_BUFFER, false);
	test_php_partial_response_timeout();
	init_mutexes();
	test_php_command(4);
	test_php_command(BUFSIZE - 3);
	test_invalid_php_commands();
	test_php_startup(argv[0]);
	test_script_execution();
	puts("production regression tests passed");
	return 0;
}
