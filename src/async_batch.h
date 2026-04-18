#ifndef SPINE_ASYNC_BATCH_H
#define SPINE_ASYNC_BATCH_H

#include <uv.h>
#include <mysql.h>

/**
 * Initialize the asynchronous SQL batching system.
 * This starts the periodic flush timer on the main loop.
 */
int spine_async_batch_init(MYSQL *mysql, int max_batch_size, int flush_interval_ms);

/**
 * Enqueue a SQL query to be executed as part of the next batch.
 * The query string must be null-terminated.
 */
int spine_async_batch_enqueue(const char *query);

/**
 * Force an immediate flush of the current batch queue.
 * Useful during shutdown.
 */
void spine_async_batch_flush(void);

/**
 * Tear down the batching system and close the timer handle.
 */
void spine_async_batch_cleanup(void);

#endif
