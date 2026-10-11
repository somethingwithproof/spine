/* Characterize the real implementations without a database or child process.
 * Keep expected quirks in the fixture; a C17 refactor must not rewrite them. */
#include "circuit_breaker.h"
#include "common.h"
#include "platform/platform_fd.h"
#include "spine.h"
#include "test_platform_helpers.h"

static char trace_buffer[262144];
static size_t trace_length;
static int clock_ticks, ping_calls, status_calls, release_calls, php_calls;
static int mock_status = HOST_UP, mock_wait = 1, mock_wait_error,
           mock_wait_calls;
static int popen_calls, pclose_calls, killed_pid, killed_signal, script_busy;
static int script_lock_attempts, mock_php_closes;
static const char *script_output = "42\n", *php_output = "42\n";
static char host_method[16], script_action[16];
static char *host_row[37], *item_row[21];
static pool_t connection = {.id = 9};
static pool_t remote_connection = {.id = 10};
static int missing_connection;
extern int *debug_devices;
static int debug_sentinel[] = {0};
spine_sem_t available_threads, available_scripts;
poller_thread_t **details;
static poller_thread_t detail;
static poller_thread_t *detail_ptr = &detail;

static void trace(const char *format, ...) {
  va_list args;
  va_start(args, format);
  int count = vsnprintf(trace_buffer + trace_length,
                        sizeof(trace_buffer) - trace_length, format, args);
  va_end(args);
  if (count < 0 || (size_t)count >= sizeof(trace_buffer) - trace_length)
    abort();
  trace_length += (size_t)count;
}

typedef struct {
  MYSQL_ROW row;
  int count;
  int fetched;
} mock_result_t;
static mock_result_t host_result, item_result, empty_result, setting_result;
static char *setting_row[] = {"setting value", NULL};
static MYSQL_RES *behavior_query(MYSQL *mysql, int mode, const char *query) {
  (void)mysql;
  trace("QUERY[%d] %s\n", mode, query);
  mock_result_t *result = &empty_result;
  if (strstr(query, "FROM host WHERE") != NULL)
    result = &host_result;
  else if (strstr(query, "FROM poller_item") != NULL &&
           strstr(query, "SELECT") != NULL)
    result = &item_result;
  else if (strstr(query, "FROM settings") != NULL)
    result = &setting_result;
  result->fetched = 0;
  return (MYSQL_RES *)result;
}
static int behavior_insert(MYSQL *mysql, int mode, const char *query) {
  (void)mysql;
  trace("INSERT[%d] %s\n", mode, query);
  return TRUE;
}
static my_ulonglong behavior_num_rows(MYSQL_RES *result) {
  return (my_ulonglong)((mock_result_t *)result)->count;
}
static MYSQL_ROW behavior_fetch_row(MYSQL_RES *result) {
  mock_result_t *r = (mock_result_t *)result;
  return r->fetched++ < r->count ? r->row : NULL;
}
static void behavior_free_result(MYSQL_RES *result) { (void)result; }
static void behavior_php_close(int process) {
  mock_php_closes++;
  trace("PHP_CLOSE[%d]\n", process);
}

#define db_query behavior_query
#define db_insert behavior_insert
#define db_free_result behavior_free_result
#define mysql_num_rows behavior_num_rows
#define mysql_fetch_row behavior_fetch_row
#define php_close behavior_php_close
#include "../../src/util.c"

static int behavior_log(const char *format, ...) {
  /* Capture only the user-facing buffered-error contract here. */
  if (strstr(format, "Invalid Response(s)") != NULL) {
    char line[DBL_BUFSIZE + SMALL_BUFSIZE];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    trace("LOG %s\n", line);
  }
  return TRUE;
}
static double behavior_clock(void) { return 1000.0 + clock_ticks++ / 1000.0; }
static int behavior_ping(host_t *host, ping_t *ping) {
  (void)host;
  ping_calls++;
  snprintf(ping->ping_status, sizeof(ping->ping_status), "1.00000");
  snprintf(ping->snmp_status, sizeof(ping->snmp_status), "2.00000");
  return mock_status;
}
static void behavior_status(int status, host_t *host, ping_t *ping,
                            int method) {
  (void)ping;
  status_calls++;
  trace("STATUS method=%d result=%d\n", method, status);
  host->status = status;
  host->total_polls++;
  host->failed_polls += status == HOST_DOWN;
  snprintf(host->status_fail_date, sizeof(host->status_fail_date), "123");
  snprintf(host->status_rec_date, sizeof(host->status_rec_date), "456");
}
static int behavior_wait(int fd, struct timeval *timeout) {
  (void)fd;
  (void)timeout;
  mock_wait_calls++;
  if (mock_wait_error == EINTR && mock_wait_calls == 1)
    return -1;
  return mock_wait;
}
static int behavior_fd_error(void) { return mock_wait_error; }
static ssize_t behavior_read(int fd, void *buf, size_t size) {
  (void)fd;
  size_t length = strlen(script_output);
  if (length > size)
    length = size;
  memcpy(buf, script_output, length);
  return (ssize_t)length;
}
static void behavior_sleep(unsigned int microseconds) { (void)microseconds; }
static int behavior_kill(pid_t pid, int signal_number) {
  killed_pid = (int)pid;
  killed_signal = signal_number;
  return 0;
}
static int behavior_trywait(spine_sem_t *semaphore) {
  script_lock_attempts++;
  if (script_busy) {
    errno = EAGAIN;
    return -1;
  }
  return spine_sem_trywait(semaphore);
}
static void behavior_thread_end(void) { trace("MYSQL_THREAD_END\n"); }

#define spine_log behavior_log
#define get_time_as_double behavior_clock
#define ping_host behavior_ping
#define update_host_status behavior_status
#define spine_fd_wait_readable behavior_wait
#define spine_fd_last_error behavior_fd_error
#define spine_fd_read behavior_read
#define spine_platform_sleep_us behavior_sleep
#define kill behavior_kill
#define spine_sem_trywait behavior_trywait
#define mysql_thread_end behavior_thread_end
#include "../../src/poller.c"
#undef spine_log

pool_t *db_get_connection(int mode) {
  if (missing_connection == mode + 1)
    return NULL;
  return mode == LOCAL ? &connection : &remote_connection;
}
void db_release_connection(int mode, int id) {
  release_calls++;
  trace("RELEASE[%d] %d\n", mode, id);
}
int spine_cb_should_skip(int id) {
  (void)id;
  return FALSE;
}
void spine_cb_record(int id, int errors) {
  trace("BREAKER[%d] %d\n", id, errors);
}
int php_get_process(void) { return 0; }
char *php_cmd(const char *command, int process) {
  php_calls++;
  trace("PHP[%d] %s\n", process, command);
  return strdup(php_output);
}
int nft_popen(const char *command, const char *type) {
  (void)command;
  (void)type;
  popen_calls++;
  return 7;
}
int nft_pclose(int fd) {
  (void)fd;
  pclose_calls++;
  return 0;
}
int nft_pchild(int fd) {
  (void)fd;
  return 12345;
}
char *snmp_get(host_t *host, const char *oid) {
  (void)host;
  (void)oid;
  return strdup("1");
}
char *snmp_getnext(host_t *host, const char *oid) {
  return snmp_get(host, oid);
}
char *snmp_get_base(host_t *host, const char *oid, bool fail) {
  (void)fail;
  return snmp_get(host, oid);
}
int snmp_count(host_t *host, const char *oid) {
  (void)host;
  (void)oid;
  return 0;
}
void snmp_get_multi(host_t *host, target_t *items, snmp_oids_t *oids,
                    int count) {
  (void)host;
  (void)items;
  for (int i = 0; i < count; i++)
    snprintf(oids[i].result, sizeof(oids[i].result), "1");
}
void *snmp_host_init(int id, char *hostname, int version, char *community,
                     char *username, char *password, char *auth,
                     char *passphrase, char *priv, char *context, char *engine,
                     int port, int timeout) {
  (void)id;
  (void)hostname;
  (void)version;
  (void)community;
  (void)username;
  (void)password;
  (void)auth;
  (void)passphrase;
  (void)priv;
  (void)context;
  (void)engine;
  (void)port;
  (void)timeout;
  return &connection;
}
void snmp_host_cleanup(void *session) { (void)session; }

static void reset_scenario(const char *label) {
  trace("SCENARIO %s\n", label);
  set = (config_t){.active_profiles = 1,
                   .total_snmp_ports = 1,
                   .script_timeout = 2,
                   .threads = 1,
                   .stdout_notty = 1,
                   .stderr_notty = 1};
  debug_devices = debug_sentinel;
  clock_ticks = ping_calls = status_calls = release_calls = php_calls = 0;
  popen_calls = pclose_calls = killed_pid = killed_signal = mock_wait_calls = 0;
  script_lock_attempts = script_busy = mock_wait_error = missing_connection = 0;
  mock_wait = 1;
  mock_status = HOST_UP;
  script_output = php_output = "42\n";
  memset(host_row, 0, sizeof(host_row));
  memset(item_row, 0, sizeof(item_row));
  host_row[0] = "7";
  host_row[1] = "127.0.0.1";
  host_row[3] = "0";
  host_row[14] = host_method;
  host_row[15] = "3";
  host_row[21] = "123";
  host_row[22] = "456";
  item_row[0] = script_action;
  item_row[1] = "127.0.0.1";
  item_row[6] = "value";
  item_row[8] = "/usr/bin/true";
  item_row[11] = "10";
  host_result = (mock_result_t){.row = host_row, .count = 1};
  item_result = (mock_result_t){.row = item_row, .count = 0};
  empty_result = (mock_result_t){0};
  setting_result = (mock_result_t){.row = setting_row, .count = 1};
  detail = (poller_thread_t){.host_threads = 1};
  details = &detail_ptr;
  snprintf(host_method, sizeof(host_method), "%d", AVAIL_PING);
  snprintf(script_action, sizeof(script_action), "%d",
           POLLER_ACTION_PHP_SCRIPT_SERVER);
}

static void test_availability(void) {
  for (int method = AVAIL_NONE; method <= AVAIL_STREAM; method++) {
    for (int up = 0; up <= 1; up++) {
      char label[80];
      snprintf(label, sizeof(label), "availability-%d-%d", method, up);
      reset_scenario(label);
      set.ping_only = 1;
      mock_status = up ? HOST_UP : HOST_DOWN;
      snprintf(host_method, sizeof(host_method), "%d", method);
      int errors = 99;
      poll_host(0, 7, 1, 1, 0, "1000", &errors, 999.0);
      const bool bypass = method == AVAIL_STREAM || method == AVAIL_SNMP;
      ASSERT_INT_EQ(ping_calls, bypass ? 0 : 1);
      ASSERT_INT_EQ(status_calls, 1);
      ASSERT_INT_EQ(release_calls, 1);
      /* ping-only returns before writing through the host_errors pointer. */
      ASSERT_INT_EQ(errors, 99);
    }
  }
  reset_scenario("secondary-thread");
  set.ping_only = 1;
  int errors = 0;
  poll_host(0, 7, 2, 2, 0, "1000", &errors, 999.0);
  ASSERT_INT_EQ(status_calls, 0);
}

static void test_output_and_writes(void) {
  const char *values[] = {"42\n", "a:1 b:2", "FF FF", "temperature=12.5 C",
                          "U",    ""};
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
    char label[80];
    snprintf(label, sizeof(label), "php-output-%zu", i);
    reset_scenario(label);
    item_result.count = 1;
    php_output = values[i];
    int errors = 0;
    poll_host(0, 7, 1, 1, 0, "1000", &errors, 999.0);
    ASSERT_INT_EQ(php_calls, 1);
    ASSERT_INT_EQ(release_calls, 1);
    ASSERT_TRUE(detail.complete);
  }
  reset_scenario("remote-boost-mysql8");
  item_result.count = 1;
  set.poller_id = 2;
  set.mode = REMOTE_ONLINE;
  set.boost_redirect = set.boost_enabled = set.dbonupdate = 1;
  int errors = 0;
  poll_host(0, 7, 1, 1, 0, "1000", &errors, 999.0);
  ASSERT_INT_EQ(release_calls, 2);
  reset_scenario("next-profile");
  set.active_profiles = 2;
  set.poller_interval = 60;
  poll_host(0, 7, 1, 1, 0, "1000", &errors, 999.0);
  reset_scenario("missing-local-connection");
  missing_connection = 1;
  poll_host(0, 7, 1, 1, 0, "1000", &errors, 999.0);
  ASSERT_INT_EQ(release_calls, 0);
  reset_scenario("missing-remote-connection");
  missing_connection = 2;
  set.poller_id = 2;
  set.mode = REMOTE_ONLINE;
  poll_host(0, 7, 1, 1, 0, "1000", &errors, 999.0);
  ASSERT_INT_EQ(release_calls, 1);
  reset_scenario("three-output-rows");
  item_result.count = 3;
  poll_host(0, 7, 1, 1, 0, "1000", &errors, 999.0);
  ASSERT_INT_EQ(php_calls, 3);
  reset_scenario("script-output-row");
  item_result.count = 1;
  snprintf(script_action, sizeof(script_action), "%d", POLLER_ACTION_SCRIPT);
  poll_host(0, 7, 1, 1, 0, "1000", &errors, 999.0);
  ASSERT_INT_EQ(popen_calls, 1);
  ASSERT_INT_EQ(pclose_calls, 1);
}

static void test_script_contract(void) {
  host_t host = {.id = 7};
  char command[] = "/usr/bin/true";
  const char *names[] = {"script-result", "script-timeout", "script-eintr",
                         "script-empty", "script-lock-busy"};
  for (size_t i = 0; i < 5; i++) {
    reset_scenario(names[i]);
    mock_wait = i == 1 ? 0 : 1;
    mock_wait_error = i == 2 ? EINTR : 0;
    script_output = i == 3 ? "" : "42\nignored\n";
    script_busy = i == 4;
    char *result = exec_poll(&host, command, 10, "DS");
    trace("SCRIPT_RESULT <%s> opens=%d closes=%d waits=%d lock_attempts=%d "
          "killed=%d/%d\n",
          result, popen_calls, pclose_calls, mock_wait_calls,
          script_lock_attempts, killed_pid, killed_signal);
    ASSERT_TRUE(
        strcmp(result,
               i == 4 ? "" : (i == 1 || i == 3 ? "U" : "42\nignored\n")) == 0);
    ASSERT_INT_EQ(pclose_calls, i == 4 ? 0 : 1);
    ASSERT_INT_EQ(killed_pid, i == 1 ? 12345 : 0);
    free(result);
  }
}

static void test_utilities(void) {
  reset_scenario("settings-write");
  MYSQL mysql = {0};
  putsetting(&mysql, LOCAL, "example", "value");
  set.dbonupdate = 1;
  putsetting(&mysql, REMOTE, "example", "value");
  set_option("Override", "first");
  set_option("override", "second");
  char *value = getsetting(&mysql, LOCAL, "OVERRIDE");
  ASSERT_TRUE(strcmp(value, "first") == 0);
  free(value);
  value = getsetting(&mysql, LOCAL, "database_setting");
  ASSERT_TRUE(strcmp(value, "setting value") == 0);
  free(value);
  reset_scenario("strings");
  const char *inputs[] = {"42", " 42 ",  "\"42\"",  "+1.2e3",   "NaN", "inf",
                          "",   "3junk", "a:1 b:2", "a:1  b:2", "a!U", "FF FF"};
  for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
    char value_buf[80];
    snprintf(value_buf, sizeof(value_buf), "%s", inputs[i]);
    int numeric = is_numeric(value_buf);
    const int multipart = is_multipart_output(value_buf);
    const int valid = validate_result(value_buf);
    trace("VALUE <%s> numeric=%d multipart=%d valid=%d mutated=<%s>\n",
          inputs[i], numeric, multipart, valid, value_buf);
  }
  char quoted[] = " \"abc\" \r\n";
  ASSERT_TRUE(strcmp(trim(quoted), "abc") == 0);
  char reversed[] = "abcd";
  ASSERT_TRUE(strcmp(reverse(reversed), "dcba") == 0);
  char hex[] = "FF FF";
  ASSERT_TRUE(hex2dec(hex) == 65535);
  ASSERT_TRUE(strcmp(hex, "FF FF") == 0);
  ASSERT_INT_EQ(strpos("hello", "ll"), 2);
  ASSERT_INT_EQ(char_count("aba", 'a'), 2);
  char bounded[4];
  strncopy(bounded, "abcdef", sizeof(bounded));
  ASSERT_TRUE(strcmp(bounded, "abc") == 0);

  char empty[] = "";
  char single[] = "x";
  char odd[] = "abc";
  ASSERT_TRUE(reverse(empty) == empty && empty[0] == '\0');
  ASSERT_TRUE(strcmp(reverse(single), "x") == 0);
  ASSERT_TRUE(strcmp(reverse(odd), "cba") == 0);
  ASSERT_TRUE(rtrim(NULL) == NULL && ltrim(NULL) == NULL);
  char all_trim[] = " \"'\\\t\r\n";
  ASSERT_TRUE(strcmp(rtrim(all_trim), "") == 0);
  const char *alpha[] = {"", "abc", "+12 C", "-12 C", "abc1", "value12.5C"};
  const char *stripped[] = {"", "", "12", "-12", "1", "12.5"};
  for (size_t i = 0; i < sizeof(alpha) / sizeof(alpha[0]); i++) {
    char input[32];
    snprintf(input, sizeof(input), "%s", alpha[i]);
    ASSERT_TRUE(strcmp(strip_alpha(input), stripped[i]) == 0);
  }
  const char unterminated[] = {'a', 'b', 'c'};
  strncopy(bounded, unterminated, sizeof(bounded));
  ASSERT_TRUE(strcmp(bounded, "abc") == 0);
  strncopy(bounded, "ignored", 0);
  ASSERT_TRUE(strcmp(bounded, "abc") == 0);
  strncopy(bounded, "ignored", 1);
  ASSERT_TRUE(bounded[0] == '\0');
  for (int format = GD_MIN; format <= GD_MAX; format++) {
    set.log_datetime_format = format;
    set.log_datetime_separator = GDC_HYPHEN;
    char *fmt = get_date_format();
    trace("DATE[%d] %s\n", format, fmt);
    free(fmt);
  }
  reset_scenario("error-buffer");
  char error_string[DBL_BUFSIZE] = {0};
  int size = 0, errors = 0;
  buffer_output_errors(error_string, &size, &errors, 7, 1, 10, false);
  buffer_output_errors(error_string, &size, &errors, 7, 1, 20, false);
  buffer_output_errors(error_string, &size, &errors, 7, 1, 0, true);
  ASSERT_TRUE(strcmp(error_string, "10, 20") == 0);
  ASSERT_INT_EQ(errors, 2);
}

typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  int ready;
} regex_sync_t;
typedef struct {
  regex_sync_t *sync;
  const char *input;
  const char *expected;
  bool matched;
} regex_worker_t;
static void *regex_worker(void *opaque) {
  regex_worker_t *worker = opaque;
  const char *result = regex_replace("[0-9][0-9]*", worker->input);
  pthread_mutex_lock(&worker->sync->mutex);
  worker->sync->ready++;
  pthread_cond_broadcast(&worker->sync->condition);
  while (worker->sync->ready < 2)
    pthread_cond_wait(&worker->sync->condition, &worker->sync->mutex);
  pthread_mutex_unlock(&worker->sync->mutex);
  worker->matched = strcmp(result, worker->expected) == 0;
  return NULL;
}
static void test_regex_and_config(void) {
  reset_scenario("regex-threads");
  regex_sync_t sync = {.mutex = PTHREAD_MUTEX_INITIALIZER,
                       .condition = PTHREAD_COND_INITIALIZER};
  regex_worker_t workers[] = {
      {.sync = &sync, .input = "value123", .expected = "123"},
      {.sync = &sync, .input = "value456", .expected = "456"},
  };
  pthread_t threads[2];
  for (size_t i = 0; i < 2; i++)
    ASSERT_INT_EQ(pthread_create(&threads[i], NULL, regex_worker, &workers[i]),
                  0);
  for (size_t i = 0; i < 2; i++) {
    ASSERT_INT_EQ(pthread_join(threads[i], NULL), 0);
    ASSERT_TRUE(workers[i].matched);
  }
  pthread_mutex_destroy(&sync.mutex);
  pthread_cond_destroy(&sync.condition);
  const char *unmatched = "abc";
  ASSERT_TRUE(regex_replace("[", unmatched) == unmatched);
  ASSERT_TRUE(regex_replace("[0-9]", unmatched) == unmatched);
  trace(
      "REGEX storage=per-thread invalid-pattern=original no-match=original\n");
  reset_scenario("configuration");
  char path[] = "/tmp/spine-behavior-config-XXXXXX";
  int fd = mkstemp(path);
  ASSERT_TRUE(fd >= 0);
  if (fd < 0)
    return;
  ASSERT_INT_EQ(fchmod(fd, S_IRUSR | S_IWUSR), 0);
  FILE *config = fdopen(fd, "w");
  ASSERT_TRUE(config != NULL);
  if (config == NULL) {
    close(fd);
    unlink(path);
    return;
  }
  fputs("# ignored\nDB_Host localhost\nDB_Port 3307\nPoller 8", config);
  fclose(config);
  ASSERT_INT_EQ(read_spine_config(path), 0);
  ASSERT_TRUE(strcmp(set.db_host, "localhost") == 0);
  ASSERT_INT_EQ(set.db_port, 3307);
  ASSERT_INT_EQ(set.poller_id, 0); /* The unterminated final line is ignored. */
  trace("CONFIG host=%s port=%u final-line-poller=%d\n", set.db_host,
        set.db_port, set.poller_id);
  unlink(path);
}

static void test_log_formatting(void) {
  reset_scenario("logging");
  set.log_level = POLLER_VERBOSITY_LOW;
  set.stdout_notty = set.stderr_notty = 0;
  set.log_destination = LOGDEST_FILE;
  for (int warning = 0; warning <= 1; warning++) {
    const int fd = warning ? STDERR_FILENO : STDOUT_FILENO;
    FILE *output = tmpfile();
    ASSERT_TRUE(output != NULL);
    if (output == NULL)
      return;
    int saved = dup(fd);
    ASSERT_TRUE(saved >= 0);
    fflush(warning ? stderr : stdout);
    ASSERT_TRUE(dup2(fileno(output), fd) >= 0);
    set.log_format = LOGFMT_JSON;
    spine_log("%s: probe \"quoted\"", warning ? "WARNING" : "INFO");
    fflush(warning ? stderr : stdout);
    dup2(saved, fd);
    close(saved);
    rewind(output);
    char line[2048] = {0};
    fread(line, 1, sizeof(line) - 1, output);
    fclose(output);
    ASSERT_TRUE(strstr(line, warning ? "\"level\":\"WARN\""
                                     : "\"level\":\"INFO\"") != NULL);
    ASSERT_TRUE(strstr(line, "\"poller\":0") != NULL);
    ASSERT_TRUE(strstr(line, "\"pid\":") != NULL &&
                strstr(line, "\"tid\":") != NULL);
    ASSERT_TRUE(strstr(line, "\\\"quoted\\\"") != NULL);
    ASSERT_TRUE(line[strlen(line) - 1] == '\n');
    trace("JSON_LOG stream=%s level=%s fields=ts,level,poller,pid,tid,msg "
          "escaped-quotes=1 newline=1\n",
          warning ? "stderr" : "stdout", warning ? "WARN" : "INFO");
  }
}

static int expected_php_closes;
static void verify_fatal_cleanup(void) {
  if (mock_php_closes != expected_php_closes)
    _Exit(99);
}

static void test_cleanup(void) {
  reset_scenario("cleanup");
  int count;
  child_cleanup_thread(NULL);
  spine_sem_getvalue(&available_threads, &count);
  ASSERT_INT_EQ(count, 1);
  spine_sem_getvalue(&available_scripts, &count);
  ASSERT_INT_EQ(count, 1);
  child_cleanup_script(NULL);
  spine_sem_getvalue(&available_scripts, &count);
  ASSERT_INT_EQ(count, 2);

  reset_scenario("worker-cleanup");
  set.ping_only = 1;
  spine_sem_t started;
  ASSERT_INT_EQ(spine_sem_init(&started, 0), 0);
  poller_thread_t worker = {.device_counter = 0,
                            .host_id = 7,
                            .host_thread = 1,
                            .host_threads = 1,
                            .thread_init_sem = &started};
  snprintf(worker.host_time, sizeof(worker.host_time), "1000");
  pthread_t thread;
  ASSERT_INT_EQ(pthread_create(&thread, NULL, child, &worker), 0);
  void *returned;
  ASSERT_INT_EQ(pthread_join(thread, &returned), 0);
  ASSERT_TRUE(returned == NULL);
  spine_sem_getvalue(&started, &count);
  ASSERT_INT_EQ(count, 1);
  spine_sem_getvalue(&available_threads, &count);
  ASSERT_INT_EQ(count, 2);
  spine_sem_destroy(&started);
  /* Capture exit code, message suffix, and parent-only PHP cleanup. */
  for (int parent = 0; parent <= 1; parent++) {
    int output[2];
    ASSERT_INT_EQ(pipe(output), 0);
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
      close(output[0]);
      dup2(output[1], STDERR_FILENO);
      close(output[1]);
      set.parent_fork = parent ? SPINE_PARENT : 0;
      set.php_initialized = 1;
      set.exit_code = 23;
      expected_php_closes = parent;
      atexit(verify_fatal_cleanup);
      die("test fatal");
    }
    close(output[1]);
    char message[128] = {0};
    ssize_t length = read(output[0], message, sizeof(message) - 1);
    close(output[0]);
    ASSERT_TRUE(length > 0);
    int status;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_INT_EQ(WEXITSTATUS(status), 23);
    ASSERT_TRUE(strcmp(message, "test fatal (Spine init)") == 0);
    trace("FATAL parent=%d exit=%d message=<%s> php_cleanup=%d\n", parent,
          WEXITSTATUS(status), message, parent);
  }
}

int main(int argc, char **argv) {
  ASSERT_INT_EQ(spine_sem_init(&available_threads, 0), 0);
  ASSERT_INT_EQ(spine_sem_init(&available_scripts, 1), 0);
  test_availability();
  test_output_and_writes();
  test_script_contract();
  test_utilities();
  test_regex_and_config();
  test_log_formatting();
  test_cleanup();
  spine_sem_destroy(&available_threads);
  spine_sem_destroy(&available_scripts);
  if (argc == 2 && strcmp(argv[1], "--capture") == 0) {
    fputs(trace_buffer, stdout);
    return test_failures ? EXIT_FAILURE : EXIT_SUCCESS;
  }
  ASSERT_INT_EQ(argc, 2);
  if (argc == 2) {
    FILE *fixture = fopen(argv[1], "rb");
    ASSERT_TRUE(fixture != NULL);
    if (fixture != NULL) {
      char expected[sizeof(trace_buffer)];
      size_t length = fread(expected, 1, sizeof(expected) - 1, fixture);
      expected[length] = '\0';
      fclose(fixture);
      ASSERT_TRUE(strcmp(expected, trace_buffer) == 0);
    }
  }
  return finish_tests("poller/util behavior");
}
