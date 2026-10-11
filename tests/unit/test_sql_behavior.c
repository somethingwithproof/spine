/* Exercise the real SQL retry and pool code without a database connection. */
#include "common.h"
#include "spine.h"
#include "test_platform_helpers.h"
#include <setjmp.h>

static int failures_left, scripted_error, query_calls, sleeps, reconnects,
    side_queries;
static unsigned long thread_id = 10;
static jmp_buf fatal_jump;
static int fatal_status;
static MYSQL_RES fake_rows;
static char last_query[DBL_BUFSIZE];

static int fake_query(MYSQL *mysql, const char *query) {
  (void)mysql;
  if (strncmp(query, "KILL ", 5) == 0 ||
      strncmp(query, "SET SESSION", 11) == 0) {
    side_queries++;
    return 0;
  }
  snprintf(last_query, sizeof(last_query), "%s", query);
  query_calls++;
  if (failures_left > 0) {
    failures_left--;
    return 1;
  }
  return 0;
}
static unsigned int fake_errno(MYSQL *mysql) {
  (void)mysql;
  return scripted_error;
}
static const char *fake_error(MYSQL *mysql) {
  (void)mysql;
  return "test error";
}
static MYSQL_RES *fake_result(MYSQL *mysql) {
  (void)mysql;
  return &fake_rows;
}
static unsigned long fake_thread(MYSQL *mysql) {
  (void)mysql;
  return thread_id;
}
static int fake_ping(MYSQL *mysql) {
  (void)mysql;
  thread_id++;
  reconnects++;
  return 0;
}
static void fake_sleep_us(unsigned int us) {
  ASSERT_INT_EQ(us, 50000);
  sleeps++;
}
static void fake_sleep_s(unsigned int seconds) { ASSERT_INT_EQ(seconds, 1); }
static _Noreturn void fake_exit(int status) {
  fatal_status = status;
  longjmp(fatal_jump, 1);
}
int spine_log(const char *format, ...) {
  (void)format;
  return 0;
}
void die(const char *format, ...) {
  (void)format;
  fake_exit(2);
}
#define mysql_query fake_query
#define mysql_errno fake_errno
#define mysql_error fake_error
#define mysql_store_result fake_result
#define mysql_thread_id fake_thread
#define mysql_ping fake_ping
#define spine_platform_sleep_us fake_sleep_us
#define spine_platform_sleep_s fake_sleep_s
#define exit fake_exit
#include "../../src/sql.c"
#undef exit

static void reset(int error, int failures) {
  set = (config_t){0};
  scripted_error = error;
  failures_left = failures;
  query_calls = sleeps = reconnects = side_queries = fatal_status = 0;
  errno = 0;
}
int main(void) {
  MYSQL mysql = {0};
  const char *query = "INSERT INTO poller_output VALUES (1,1,NOW(),'42')";
  reset(0, 0);
  set.dry_run = TRUE;
  ASSERT_INT_EQ(db_insert(NULL, LOCAL, query), TRUE);
  ASSERT_INT_EQ(query_calls, 0);
  set.dry_run = FALSE;
  set.SQL_readonly = TRUE;
  ASSERT_INT_EQ(db_insert(NULL, REMOTE, query), TRUE);
  ASSERT_INT_EQ(query_calls, 0);
  reset(0, 0);
  ASSERT_INT_EQ(db_insert(&mysql, LOCAL, query), TRUE);
  ASSERT_TRUE(strcmp(last_query, query) == 0);
  for (int code = 1205; code <= 1213; code += 8) {
    reset(code, 2);
    ASSERT_INT_EQ(db_insert(&mysql, LOCAL, query), TRUE);
    ASSERT_INT_EQ(query_calls, 3);
    ASSERT_INT_EQ(sleeps, 2);
    reset(code, 100);
    ASSERT_INT_EQ(db_insert(&mysql, REMOTE, query), FALSE);
    ASSERT_INT_EQ(query_calls, 31);
    ASSERT_INT_EQ(sleeps, 31);
    reset(code, 31);
    if (setjmp(fatal_jump) == 0) {
      db_query(&mysql, LOCAL, "SELECT 42");
      ASSERT_FAIL("query must exit");
    }
    ASSERT_INT_EQ(fatal_status, 1);
    ASSERT_INT_EQ(query_calls, 31);
  }
  for (int code = 2006; code <= 2013; code += 7) {
    reset(code, 2);
    ASSERT_INT_EQ(db_insert(&mysql, LOCAL, query), TRUE);
    ASSERT_INT_EQ(query_calls, 3);
    ASSERT_INT_EQ(reconnects, 2);
    ASSERT_INT_EQ(side_queries, 14);
    reset(code, 35);
    errno = EINTR;
    ASSERT_TRUE(db_query(&mysql, REMOTE, "SELECT 42") == &fake_rows);
    ASSERT_INT_EQ(query_calls, 36);
    ASSERT_INT_EQ(sleeps, 35);
    ASSERT_INT_EQ(reconnects, 0);
    reset(code, 31);
    if (setjmp(fatal_jump) == 0) {
      db_insert(&mysql, LOCAL, query);
      ASSERT_FAIL("reconnect limit must die");
    }
    ASSERT_INT_EQ(fatal_status, 2);
    ASSERT_INT_EQ(reconnects, 31);
  }
  reset(1064, 1);
  ASSERT_INT_EQ(db_insert(&mysql, LOCAL, query), FALSE);
  ASSERT_INT_EQ(query_calls, 1);
  reset(1064, 1);
  if (setjmp(fatal_jump) == 0) {
    db_query(&mysql, LOCAL, "bad query");
    ASSERT_FAIL("nonretryable query must exit");
  }
  ASSERT_INT_EQ(fatal_status, 1);
  reset(0, 0);
  pool_t local[2] = {{.free = TRUE}, {.free = TRUE}};
  pool_t remote[2] = {{.free = TRUE}, {.free = TRUE}};
  set.threads = 2;
  db_pool_local = local;
  db_pool_remote = remote;
  ASSERT_TRUE(db_get_connection(LOCAL) == &local[0]);
  ASSERT_TRUE(db_get_connection(LOCAL) == &local[1]);
  ASSERT_TRUE(db_get_connection(LOCAL) == NULL);
  ASSERT_TRUE(db_get_connection(REMOTE) == &remote[0]);
  db_release_connection(LOCAL, 0);
  ASSERT_TRUE(db_get_connection(LOCAL) == &local[0]);
  db_pool_local = db_pool_remote = NULL;
  return finish_tests("SQL behavior");
}
