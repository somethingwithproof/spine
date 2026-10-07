/* spine_log() stack use.
 *
 * Poller threads run on the platform's default stack: 128 KiB on musl,
 * 512 KiB on macOS.  The logging cases run on a thread of the musl size in a
 * child process, so an overflow is a reported failure rather than a crash of
 * the runner.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>

#include "common.h"
#include "spine.h"

#define MUSL_DEFAULT_STACK (128 * 1024)

static char log_path[] = "spine-log-stack-XXXXXX";

static void *log_from_thread(void *argument) {
	char *message;

	(void) argument;
	message = malloc(LOGSIZE);
	if (message == NULL) return argument;
	memset(message, 'x', LOGSIZE - 1);
	message[LOGSIZE - 1] = '\0';

	set.logging.log_destination = LOGDEST_FILE;
	spine_log("stack check %s", message);
	set.logging.log_destination = LOGDEST_STDOUT;
	spine_log("stack check %s", message);

	free(message);
	return NULL;
}

static void test_spine_log_fits_a_musl_thread_stack(void **state) {
	pid_t child;
	int status;
	int fd;

	(void) state;
	fd = mkstemp(log_path);
	assert_true(fd >= 0);
	assert_int_equal(close(fd), 0);

	fflush(NULL);
	child = fork();
	assert_true(child >= 0);

	if (child == 0) {
		pthread_attr_t attr;
		pthread_t thread;
		void *result = &status;

		/* cmocka traps SIGSEGV; let an overflow kill the child visibly */
		signal(SIGSEGV, SIG_DFL);
		signal(SIGBUS, SIG_DFL);
		if (freopen("/dev/null", "w", stdout) == NULL) _exit(3);
		memset(&set, 0, sizeof(set));
		config_defaults();
		set.logging.log_level         = POLLER_VERBOSITY_LOW;
		set.logging.logfile_processed = TRUE;
		set.console.stdout_notty      = FALSE;
		set.console.stderr_notty      = TRUE;
		STRNCOPY(set.logging.path_logfile, log_path);

		if (pthread_attr_init(&attr) != 0) _exit(4);
		if (pthread_attr_setstacksize(&attr, MUSL_DEFAULT_STACK) != 0) _exit(5);
		if (pthread_create(&thread, &attr, log_from_thread, NULL) != 0) _exit(6);
		if (pthread_join(thread, &result) != 0 || result != NULL) _exit(7);
		_exit(0);
	}

	assert_int_equal(waitpid(child, &status, 0), child);
	unlink(log_path);
	if (!WIFEXITED(status)) {
		fail_msg("spine_log() killed a 128 KiB thread with signal %d", WTERMSIG(status));
	}
	assert_int_equal(WEXITSTATUS(status), 0);
}

int main(void) {
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_spine_log_fits_a_musl_thread_stack),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
