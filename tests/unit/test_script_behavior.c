/* Actual PHP framing/restart code with deterministic process and FD adapters;
 * actual nft_popen is linked separately and exercised with a real child. */
#include "common.h"
#include "nft_popen.h"
#include "platform/platform_fd.h"
#include "platform/platform_process.h"
#include "spine.h"
#include "test_platform_helpers.h"
#include <sys/wait.h>

static php_t servers[MAX_PHP_SERVERS];
php_t *php_processes = servers;
static int request_writes, write_failures, spawns, closes, terminations, waits;
static int pending_handshake, timeout_once, eof_once;
static char request[BUFSIZE], arguments[512];
static const char *reply = "42\n";
static size_t reply_offset;
int spine_log(const char *format, ...) {
  (void)format;
  return 0;
}
void die(const char *format, ...) {
  (void)format;
  abort();
}
static double fake_time(void) { return 10.0; }
static ssize_t fake_write(int fd, const void *buf, size_t count) {
  (void)fd;
  if (count == 6 && memcmp(buf, "quit\r\n", 6) == 0)
    return (ssize_t)count;
  request_writes++;
  ASSERT_TRUE(count < sizeof(request));
  memcpy(request, buf, count);
  request[count] = '\0';
  if (write_failures > 0) {
    write_failures--;
    return -1;
  }
  return (ssize_t)count;
}
static ssize_t fake_read(int fd, void *buf, size_t count) {
  (void)fd;
  if (eof_once) {
    eof_once = 0;
    return 0;
  }
  const char *value = pending_handshake ? "Started\n" : reply;
  pending_handshake = 0;
  size_t len = strlen(value + reply_offset);
  if (len > count)
    len = count;
  memcpy(buf, value + reply_offset, len);
  if (len < strlen(value + reply_offset))
    reply_offset += len;
  else
    reply_offset = 0;
  return (ssize_t)len;
}
static int fake_wait(int fd, struct timeval *timeout) {
  (void)fd;
  ASSERT_TRUE(timeout->tv_sec >= 0);
  waits++;
  if (timeout_once) {
    timeout_once = 0;
    return 0;
  }
  return 1;
}
static int fake_pipe(int fds[2]) {
  fds[0] = 10;
  fds[1] = 11;
  return 0;
}
static int fake_close(int fd) {
  (void)fd;
  closes++;
  return 0;
}
static int fake_terminate(spine_pid_t pid) {
  ASSERT_TRUE(pid > 1);
  terminations++;
  return 0;
}
static int fake_spawn(spine_pid_t *pid, const char *path,
                      posix_spawn_file_actions_t *actions,
                      posix_spawnattr_t *attr, char *const argv[],
                      char *const envp[], int retries, unsigned int delay) {
  (void)path;
  (void)actions;
  (void)attr;
  (void)envp;
  ASSERT_INT_EQ(retries, 3);
  ASSERT_INT_EQ(delay, 50000);
  arguments[0] = '\0';
  for (size_t i = 0; argv[i]; i++) {
    if (i)
      strcat(arguments, "|");
    strcat(arguments, argv[i]);
  }
  *pid = 99;
  spawns++;
  pending_handshake = 1;
  return 0;
}
#define spine_fd_write fake_write
#define spine_fd_read fake_read
#define spine_fd_wait_readable fake_wait
#define spine_process_pipe fake_pipe
#define spine_process_close_fd fake_close
#define spine_process_terminate fake_terminate
#define spine_process_spawn_retry fake_spawn
#define get_time_as_double fake_time
#include "../../src/php.c"
#undef spine_fd_write
#undef spine_fd_read
#undef spine_fd_wait_readable
#undef spine_process_pipe
#undef spine_process_close_fd
#undef spine_process_terminate
#undef spine_process_spawn_retry

static void test_php(void) {
  set = (config_t){.php_servers = 2,
                   .script_timeout = 2,
                   .cacti_version = 1223,
                   .poller_id = 2,
                   .mode = REMOTE_ONLINE};
  snprintf(set.path_php, sizeof(set.path_php), "%s", "php");
  snprintf(set.path_php_server, sizeof(set.path_php_server), "%s",
           "server.php");
  ASSERT_INT_EQ(php_init(0), TRUE);
  ASSERT_TRUE(
      strcmp(arguments,
             "php|-q|server.php|--environ=spine|--poller=2|--mode=online") ==
      0);
  ASSERT_INT_EQ(servers[0].php_state, PHP_READY);
  set.mode = REMOTE_OFFLINE;
  ASSERT_INT_EQ(php_init(0), TRUE);
  ASSERT_TRUE(strstr(arguments, "--mode=offline") != NULL);
  set.poller_id = 1;
  ASSERT_INT_EQ(php_init(0), TRUE);
  ASSERT_TRUE(
      strcmp(arguments, "php|-q|server.php|--environ=spine|--poller=1") == 0);
  set.cacti_version = 1222;
  ASSERT_INT_EQ(php_init(0), TRUE);
  ASSERT_TRUE(strcmp(arguments, "php|-q|server.php|spine|1") == 0);
  ASSERT_INT_EQ(php_get_process(), 0);
  ASSERT_INT_EQ(php_get_process(), 1);
  ASSERT_INT_EQ(php_get_process(), 0);
  char *value = php_cmd("query 1", 0);
  ASSERT_TRUE(strcmp(request, "query 1\r\n") == 0);
  ASSERT_TRUE(strcmp(value, "42\n") == 0);
  free(value);
  char long_command[BUFSIZE + 1];
  memset(long_command, 'x', sizeof(long_command) - 1);
  long_command[sizeof(long_command) - 1] = '\0';
  value = php_cmd(long_command, 0);
  ASSERT_TRUE(strlen(request) == BUFSIZE - 1);
  ASSERT_TRUE(strstr(request, "\r\n") == NULL);
  free(value);
  eof_once = 1;
  value = php_cmd("empty", 0);
  ASSERT_TRUE(strcmp(value, "U") == 0);
  free(value);
  int before = spawns;
  timeout_once = 1;
  value = php_cmd("timeout", 0);
  ASSERT_TRUE(strcmp(value, "U") == 0);
  free(value);
  ASSERT_INT_EQ(spawns, before + 1);
  /* Recovery retains the same request/restart counts after write failure. */
  before = spawns;
  write_failures = 1;
  value = php_cmd("retry", 0);
  ASSERT_TRUE(strcmp(value, "42\n") == 0);
  free(value);
  ASSERT_INT_EQ(spawns, before + 1);
  before = request_writes;
  write_failures = 3;
  value = php_cmd("failed", 0);
  ASSERT_TRUE(strcmp(value, "U") == 0);
  free(value);
  ASSERT_INT_EQ(request_writes, before + 3);
  servers[0].php_pid = 1;
  before = terminations;
  php_close(0);
  ASSERT_INT_EQ(terminations, before);
  ASSERT_INT_EQ(servers[0].php_read_fd, -1);
  ASSERT_INT_EQ(servers[0].php_write_fd, -1);
  ASSERT_TRUE(closes > 0 && waits > 0);
}
static void test_child(void) {
  /* Exercise the production environment helper with controlled values only. */
  char **saved_environment = environ;
  char safe[] = "SAFE=value", linker[] = "LD_PRELOAD=blocked";
  char apple[] = "DYLD_INSERT_LIBRARIES=blocked", shell[] = "BASH_ENV=blocked";
  char *controlled[] = {safe, linker, apple, shell, NULL};
  environ = controlled;
  char **filtered = spine_build_child_env();
  ASSERT_TRUE(filtered != NULL);
  if (filtered) {
    ASSERT_TRUE(strcmp(filtered[0], "SAFE=value") == 0);
    ASSERT_TRUE(strncmp(filtered[1], "PATH=", 5) == 0);
    ASSERT_TRUE(strncmp(filtered[2], "IFS=", 4) == 0);
    ASSERT_TRUE(filtered[3] == NULL);
    free(filtered);
  }
  environ = saved_environment;
  errno = 0;
  ASSERT_INT_EQ(nft_popen("true", "bad"), -1);
  ASSERT_INT_EQ(errno, EINVAL);
  int fd = nft_popen("printf 'hello\\n'; exit 7", "r");
  ASSERT_TRUE(fd >= 0);
  if (fd < 0)
    return;
  ASSERT_TRUE(nft_pchild(fd) > 1);
  char text[32] = {0};
  ASSERT_INT_EQ((int)spine_fd_read(fd, text, sizeof(text) - 1), 6);
  ASSERT_TRUE(strcmp(text, "hello\n") == 0);
  int status = nft_pclose(fd);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_INT_EQ(WEXITSTATUS(status), 7);
  ASSERT_INT_EQ(nft_pchild(fd), -1);
  ASSERT_INT_EQ(errno, EBADF);
  ASSERT_INT_EQ(nft_pclose(fd), -1);
  ASSERT_INT_EQ(errno, EBADF);
}
static void test_buffers(void) {
  set.script_timeout = 2;
  char *large = malloc(RESULTS_BUFFER + 1);
  ASSERT_TRUE(large != NULL);
  if (!large)
    return;
  memset(large, 'x', RESULTS_BUFFER);
  large[RESULTS_BUFFER] = '\0';
  reply = large;
  reply_offset = 0;
  char *value = php_readpipe(0, "large");
  ASSERT_TRUE(strcmp(value, "U") == 0);
  free(value);
  large[RESULTS_BUFFER - 2] = '\n';
  large[RESULTS_BUFFER - 1] = '\0';
  reply_offset = 0;
  value = php_readpipe(0, "maximum valid");
  ASSERT_TRUE(strlen(value) == RESULTS_BUFFER - 1);
  ASSERT_TRUE(value[RESULTS_BUFFER - 2] == '\n');
  free(value);
  free(large);
  reply = "42\n";
}
int main(int argc, char **argv) {
  (void)argv;
  if (argc > 1)
    test_buffers();
  else {
    test_php();
    test_child();
  }
  return finish_tests("script behavior");
}
