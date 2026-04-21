#ifndef SPINE_ASYNC_MYSQL_H
#define SPINE_ASYNC_MYSQL_H

#include <uv.h>
#include <mysql.h>

typedef void (*async_mysql_cb)(MYSQL *mysql, int status, void *data);

int spine_async_mysql_query(uv_loop_t *runtime_loop, MYSQL *mysql, const char *query, async_mysql_cb cb, void *data);

/* Set the async-mysql shutdown fence. After this call, new queries
 * submitted through spine_async_mysql_query return -ESHUTDOWN; in-
 * flight queries continue through their existing uv_poll chain so
 * the uv_run drain can flush them before the MYSQL handle is closed. */
void spine_async_mysql_shutdown_begin(void);

/* Cumulative count of queries refused after the shutdown fence was
 * set. Non-zero at process exit indicates worker-thread coordination
 * lag; operators read this via spine_dump_config or telemetry. */
unsigned long spine_async_mysql_shutdown_refused_count(void);

#ifdef SPINE_ENABLE_TEST_HOOKS
/* Test-only: clear the fence and refused counter. Production code
 * must never call this - the production fence is a one-way latch.
 * Gated behind SPINE_ENABLE_TEST_HOOKS so release builds neither
 * declare nor link the reset symbol. */
void spine_async_mysql_shutdown_reset_for_test(void);
#endif

#endif
