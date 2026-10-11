/* Real lock initialization/mappings, keyword tables and signal lifecycle. */
#include "common.h"
#include "platform/platform_process.h"
#include "spine.h"
#include "test_platform_helpers.h"
config_t set;
int spine_log(const char *format, ...) {
  (void)format;
  return 0;
}
char *get_date_format(void) { return "%Y-%m-%d"; }
int all_digits(const char *text) {
  for (; *text; text++)
    if (*text < '0' || *text > '9')
      return FALSE;
  return TRUE;
}
#include "../../src/error.c"
#include "../../src/keywords.c"
#include "../../src/locks.c"
static int spawn_calls, spawn_failures, spawn_error_code, spawn_sleeps;
static int fake_spawn(pid_t *pid, const char *path,
                      const posix_spawn_file_actions_t *file_actions,
                      const posix_spawnattr_t *attr, char *const argv[],
                      char *const envp[]) {
  (void)path;
  (void)file_actions;
  (void)attr;
  (void)argv;
  ASSERT_TRUE(envp != NULL);
  spawn_calls++;
  if (spawn_failures-- > 0)
    return spawn_error_code;
  *pid = 123;
  return 0;
}
static void fake_sleep(unsigned int us) {
  ASSERT_INT_EQ(us, 50000);
  spawn_sleeps++;
}
#define posix_spawn fake_spawn
#define spine_platform_sleep_us fake_sleep
#define spine_process_spawn_retry captured_spawn_retry
#define spine_process_pipe captured_pipe
#define spine_process_close_fd captured_close_fd
#define spine_process_wait captured_wait
#define spine_process_terminate captured_terminate
#include "../../src/platform/platform_process_posix.c"
#undef posix_spawn
#undef spine_platform_sleep_us
static void test_spawn_retries(void) {
  spine_pid_t pid = -1;
  char path[] = "test";
  char *argv[] = {path, NULL};
  for (int i = 0; i < 2; i++) {
    spawn_calls = spawn_sleeps = 0;
    spawn_failures = 2;
    spawn_error_code = i ? ENOMEM : EAGAIN;
    ASSERT_INT_EQ(
        captured_spawn_retry(&pid, path, NULL, NULL, argv, NULL, 3, 50000), 0);
    ASSERT_INT_EQ(spawn_calls, 3);
    ASSERT_INT_EQ(spawn_sleeps, 2);
    ASSERT_INT_EQ(pid, 123);
    spawn_calls = spawn_sleeps = 0;
    spawn_failures = 10;
    pid = -1;
    ASSERT_INT_EQ(
        captured_spawn_retry(&pid, path, NULL, NULL, argv, NULL, 3, 50000),
        spawn_error_code);
    ASSERT_INT_EQ(spawn_calls, 4);
    ASSERT_INT_EQ(spawn_sleeps, 3);
    ASSERT_INT_EQ(pid, -1);
  }
  spawn_calls = spawn_sleeps = 0;
  spawn_failures = 1;
  spawn_error_code = EINVAL;
  ASSERT_INT_EQ(
      captured_spawn_retry(&pid, path, NULL, NULL, argv, NULL, 3, 50000),
      EINVAL);
  ASSERT_INT_EQ(spawn_calls, 1);
  ASSERT_INT_EQ(spawn_sleeps, 0);
}
static void custom_handler(int signum) { (void)signum; }
int main(void) {
  test_spawn_retries();
  init_mutexes();
  init_mutexes();
  const int locks[] = {
      LOCK_SNMP,        LOCK_SETEUID,     LOCK_GHBN,        LOCK_POOL,
      LOCK_PHP,         LOCK_PHP_PROC_0,  LOCK_PHP_PROC_1,  LOCK_PHP_PROC_2,
      LOCK_PHP_PROC_3,  LOCK_PHP_PROC_4,  LOCK_PHP_PROC_5,  LOCK_PHP_PROC_6,
      LOCK_PHP_PROC_7,  LOCK_PHP_PROC_8,  LOCK_PHP_PROC_9,  LOCK_PHP_PROC_10,
      LOCK_PHP_PROC_11, LOCK_PHP_PROC_12, LOCK_PHP_PROC_13, LOCK_PHP_PROC_14,
      LOCK_THDET,       LOCK_HOST_TIME};
  for (size_t i = 0; i < sizeof(locks) / sizeof(locks[0]); i++) {
    ASSERT_TRUE(get_lock(locks[i]) != NULL && get_cond(locks[i]) != NULL);
    ASSERT_INT_EQ(thread_mutex_trylock(locks[i]), 0);
    ASSERT_INT_EQ(thread_mutex_trylock(locks[i]), EBUSY);
    thread_mutex_unlock(locks[i]);
  }
  ASSERT_TRUE(get_lock(-1) == NULL && get_cond(-1) == NULL &&
              get_attr(-1) == NULL);
  ASSERT_INT_EQ(parse_log_level("debug", -1), POLLER_VERBOSITY_DEBUG);
  ASSERT_INT_EQ(parse_log_level("17", -1), 17);
  ASSERT_INT_EQ(parse_logdest("both", -1), LOGDEST_BOTH);
  ASSERT_INT_EQ(parse_action("PHPSCRIPT_CT", -1),
                POLLER_ACTION_PHP_SCRIPT_SERVER_COUNT);
  ASSERT_INT_EQ(parse_action("missing", -123), -123);
  ASSERT_TRUE(strcmp(printable_action(-1), "-unknown-") == 0);
  struct sigaction saved_int, saved_pipe, actual;
  ASSERT_INT_EQ(sigaction(SIGINT, NULL, &saved_int), 0);
  ASSERT_INT_EQ(sigaction(SIGPIPE, NULL, &saved_pipe), 0);
  signal(SIGINT, SIG_DFL);
  signal(SIGPIPE, custom_handler);
  install_spine_signal_handler();
  sigaction(SIGINT, NULL, &actual);
  ASSERT_TRUE(actual.sa_handler == spine_signal_handler);
  sigaction(SIGPIPE, NULL, &actual);
  ASSERT_TRUE(actual.sa_handler == custom_handler);
  uninstall_spine_signal_handler();
  sigaction(SIGINT, NULL, &actual);
  ASSERT_TRUE(actual.sa_handler == SIG_DFL);
  sigaction(SIGPIPE, NULL, &actual);
  ASSERT_TRUE(actual.sa_handler == custom_handler);
  sigaction(SIGINT, &saved_int, NULL);
  sigaction(SIGPIPE, &saved_pipe, NULL);
  return finish_tests("support behavior");
}
