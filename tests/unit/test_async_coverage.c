#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <uv.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "../../src/spine.h"
#include "../../src/async_exec.h"
#include "../../src/async_snmp.h"
#include "../../src/async_mysql.h"
#include "test_platform_helpers.h"

// Define loop
#ifndef HAVE_LIBUV
#define HAVE_LIBUV 1
#endif
uv_loop_t *loop = NULL;

static void exec_cb(const char *result, int exit_status, int term_signal, void *data) {
    (void)result;
    (void)exit_status;
    (void)term_signal;
    int *called = (int *)data;
    *called = 1;
}

static void test_async_exec_success(void) {
    int called = 0;
    int r = spine_async_exec("echo test", 1000, exec_cb, &called);
    ASSERT_INT_EQ(r, 0);
    uv_run(loop, UV_RUN_DEFAULT);
    ASSERT_INT_EQ(called, 1);
}

static void test_async_exec_timeout(void) {
    int called = 0;
    int r = spine_async_exec("sleep 2", 10, exec_cb, &called);
    ASSERT_INT_EQ(r, 0);
    uv_run(loop, UV_RUN_DEFAULT);
    ASSERT_INT_EQ(called, 1);
}

static void test_async_exec_spawn_fail(void) {
    int called = 0;
    // Test a command that will return a non-zero exit status
    int r = spine_async_exec("exit 1", 1000, exec_cb, &called);
    ASSERT_INT_EQ(r, 0);
    uv_run(loop, UV_RUN_DEFAULT);
    ASSERT_INT_EQ(called, 1);
}

static void test_async_snmp_parse_fail(void) {
    // Test parsing failure
    int r = spine_async_snmp_get(NULL, "invalid.oid");
    ASSERT_INT_EQ(r, -1);
}

#ifndef HAVE_MYSQL_ASYNC
static void mysql_cb(MYSQL *mysql, int status, void *data) {
    (void)mysql;
    (void)status;
    int *called = (int *)data;
    *called = 1;
}

static void test_async_mysql_query(void) {
    int called = 0;
    int r = spine_async_mysql_query(NULL, "SELECT 1", mysql_cb, &called);
    ASSERT_INT_EQ(r, -1);
}
#endif

// Stubs for poller functions
void extract_and_format_pdu(struct snmp_pdu *pdu, poll_context_t *ctx) {
    (void)pdu; (void)ctx;
}

void spine_transition_state(poll_context_t *ctx) {
    (void)ctx;
}

int main(void) {
    loop = uv_default_loop();
    test_async_exec_success();
    test_async_exec_timeout();
    test_async_exec_spawn_fail();
    test_async_snmp_parse_fail();
#ifndef HAVE_MYSQL_ASYNC
    test_async_mysql_query();
#endif
    return finish_tests("async coverage tests");
}
