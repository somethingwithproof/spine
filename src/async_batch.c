#include "common.h"
#include "spine.h"
#include "async_mysql.h"
#include "async_batch.h"

#include <errno.h>
#include <stdatomic.h>

/* Async batch writer.
 *
 * Concurrency model (post-audit hardening):
 *   - Queue head/tail/pending_count/active_queries/closing/shutting_down are
 *     protected by g_batch_lock. Every mutation and read of those fields
 *     happens with the lock held.
 *   - Counter totals (dropped/enqueue_failures/submitted) are atomic_ulong
 *     so spine_async_batch_get_stats() can read them without taking the
 *     queue lock.
 *   - libuv timer and mysql submission are still invoked exclusively on
 *     the main loop thread; the lock is released around those calls to
 *     avoid holding it across I/O.
 */

typedef struct async_batch_item {
    char *query;
    struct async_batch_item *next;
} async_batch_item_t;

typedef struct {
    uv_loop_t *loop;
    MYSQL *mysql;
    uv_timer_t flush_timer;
    uv_mutex_t lock;
    async_batch_item_t *head;
    async_batch_item_t *tail;
    int pending_count;
    int max_pending;
    int flush_interval_ms;
    int active_queries;
    atomic_ulong dropped_queries;
    atomic_ulong enqueue_failures;
    atomic_ulong submitted_queries;
    bool initialized;
    bool closing;
    /* Enqueue-side shutdown fence. Flipped by spine_async_batch_cleanup
     * so concurrent worker threads cannot keep inserting after teardown
     * has committed to draining the queue. Checked without the lock on
     * the hot path; readers see the latest set via release/acquire. */
    atomic_int shutting_down;
} async_batch_ctx_t;

static async_batch_ctx_t g_batch_ctx = {0};

static void dispatch_next_locked(void);
static void dispatch_next(void);

/* Caller holds g_batch_ctx.lock. */
static async_batch_item_t *queue_pop_locked(void) {
    async_batch_item_t *item = g_batch_ctx.head;
    if (item == NULL) return NULL;
    g_batch_ctx.head = item->next;
    if (g_batch_ctx.head == NULL) g_batch_ctx.tail = NULL;
    item->next = NULL;
    g_batch_ctx.pending_count--;
    return item;
}

static void batch_query_cb(MYSQL *mysql, int status, void *data) {
    async_batch_item_t *item = (async_batch_item_t *)data;
    (void)mysql;

    if (status != 0) {
        SPINE_LOG(("ERROR: Async batch flush failed with status %d", status));
    } else {
        SPINE_LOG_DEVDBG(("DEBUG: Async batch flush succeeded"));
    }

    if (item != NULL) {
        free(item->query);
        free(item);
    }

    uv_mutex_lock(&g_batch_ctx.lock);
    if (g_batch_ctx.active_queries > 0) {
        g_batch_ctx.active_queries--;
    }
    bool may_continue = !g_batch_ctx.closing;
    uv_mutex_unlock(&g_batch_ctx.lock);

    if (may_continue) {
        dispatch_next();
    }
}

/* Caller holds g_batch_ctx.lock. Returns 0 on success, -errno on fail. */
static int queue_push_locked(const char *query) {
    if (query == NULL || *query == '\0') return -EINVAL;

    if ((g_batch_ctx.pending_count + g_batch_ctx.active_queries)
        >= g_batch_ctx.max_pending) {
        atomic_fetch_add_explicit(&g_batch_ctx.enqueue_failures, 1,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&g_batch_ctx.dropped_queries, 1,
                                  memory_order_relaxed);
        return -EAGAIN;
    }

    async_batch_item_t *item = (async_batch_item_t *)calloc(1, sizeof(*item));
    if (item == NULL) {
        atomic_fetch_add_explicit(&g_batch_ctx.enqueue_failures, 1,
                                  memory_order_relaxed);
        return -ENOMEM;
    }

    item->query = strdup(query);
    if (item->query == NULL) {
        free(item);
        atomic_fetch_add_explicit(&g_batch_ctx.enqueue_failures, 1,
                                  memory_order_relaxed);
        return -ENOMEM;
    }

    item->next = NULL;
    if (g_batch_ctx.tail != NULL) {
        g_batch_ctx.tail->next = item;
    } else {
        g_batch_ctx.head = item;
    }
    g_batch_ctx.tail = item;
    g_batch_ctx.pending_count++;
    return 0;
}

/* Caller holds g_batch_ctx.lock. Drops lock around mysql submission. */
static void dispatch_next_locked(void) {
    if (!g_batch_ctx.initialized || g_batch_ctx.closing ||
        g_batch_ctx.active_queries > 0) {
        return;
    }

    async_batch_item_t *item = queue_pop_locked();
    if (item == NULL) return;

    g_batch_ctx.active_queries++;

    /* Release the lock before calling into libuv + mysql. Holding the
     * batch lock across I/O serialises worker enqueues with the flush
     * path and re-creates the single-mutex-serialises-everything
     * contention we are avoiding. */
    uv_loop_t *loop = g_batch_ctx.loop;
    MYSQL *mysql = g_batch_ctx.mysql;
    uv_mutex_unlock(&g_batch_ctx.lock);

    int r = spine_async_mysql_query(loop, mysql, item->query, batch_query_cb, item);

    uv_mutex_lock(&g_batch_ctx.lock);
    if (r != 0) {
        SPINE_LOG(("ERROR: Async DB submit failed with status %d", r));
        if (g_batch_ctx.active_queries > 0) g_batch_ctx.active_queries--;
        free(item->query);
        free(item);
        atomic_fetch_add_explicit(&g_batch_ctx.enqueue_failures, 1,
                                  memory_order_relaxed);
        return;
    }
    atomic_fetch_add_explicit(&g_batch_ctx.submitted_queries, 1,
                              memory_order_relaxed);
}

static void dispatch_next(void) {
    uv_mutex_lock(&g_batch_ctx.lock);
    dispatch_next_locked();
    uv_mutex_unlock(&g_batch_ctx.lock);
}

static void on_flush_timer(uv_timer_t *handle) {
    (void)handle;
    dispatch_next();
}

int spine_async_batch_init(uv_loop_t *runtime_loop, MYSQL *mysql, int max_batch_size, int flush_interval_ms) {
    if (g_batch_ctx.initialized) return 0;
    if (!runtime_loop || !mysql) return -1;

    if (uv_mutex_init(&g_batch_ctx.lock) != 0) return -1;

    g_batch_ctx.loop = runtime_loop;
    g_batch_ctx.mysql = mysql;
    g_batch_ctx.max_pending = max_batch_size > 0 ? max_batch_size : 256;
    g_batch_ctx.flush_interval_ms = flush_interval_ms;
    g_batch_ctx.head = NULL;
    g_batch_ctx.tail = NULL;
    g_batch_ctx.pending_count = 0;
    g_batch_ctx.active_queries = 0;
    atomic_store_explicit(&g_batch_ctx.dropped_queries, 0, memory_order_relaxed);
    atomic_store_explicit(&g_batch_ctx.enqueue_failures, 0, memory_order_relaxed);
    atomic_store_explicit(&g_batch_ctx.submitted_queries, 0, memory_order_relaxed);
    atomic_store_explicit(&g_batch_ctx.shutting_down, 0, memory_order_release);
    g_batch_ctx.closing = false;

    uv_timer_init(g_batch_ctx.loop, &g_batch_ctx.flush_timer);
    uv_timer_start(&g_batch_ctx.flush_timer, on_flush_timer,
                   flush_interval_ms, flush_interval_ms);

    g_batch_ctx.initialized = true;
    return 0;
}

int spine_async_batch_enqueue(const char *query) {
    /* Enqueue-side shutdown fence. Workers that race past this check
     * and then see closing=true under the lock still get -EINVAL; the
     * fence closes the common case without contending on the lock. */
    if (atomic_load_explicit(&g_batch_ctx.shutting_down,
                             memory_order_acquire)) {
        atomic_fetch_add_explicit(&g_batch_ctx.enqueue_failures, 1,
                                  memory_order_relaxed);
        return -ESHUTDOWN;
    }

    uv_mutex_lock(&g_batch_ctx.lock);
    if (!g_batch_ctx.initialized || g_batch_ctx.closing) {
        uv_mutex_unlock(&g_batch_ctx.lock);
        return -EINVAL;
    }

    int rc = queue_push_locked(query);
    if (rc == 0) {
        dispatch_next_locked();
    }
    uv_mutex_unlock(&g_batch_ctx.lock);
    return rc;
}

void spine_async_batch_flush(void) {
    uv_mutex_lock(&g_batch_ctx.lock);
    bool ok = g_batch_ctx.initialized;
    if (ok) {
        dispatch_next_locked();
    }
    uv_mutex_unlock(&g_batch_ctx.lock);
}

static void on_batch_close(uv_handle_t *handle) {
    (void)handle;
    /* Zero per-session state so a future re-init starts clean; counters
     * are reset in spine_async_batch_init. */
    uv_mutex_lock(&g_batch_ctx.lock);
    g_batch_ctx.loop = NULL;
    g_batch_ctx.head = NULL;
    g_batch_ctx.tail = NULL;
    g_batch_ctx.pending_count = 0;
    g_batch_ctx.active_queries = 0;
    g_batch_ctx.initialized = false;
    g_batch_ctx.closing = false;
    uv_mutex_unlock(&g_batch_ctx.lock);
    uv_mutex_destroy(&g_batch_ctx.lock);
}

void spine_async_batch_cleanup(void) {
    /* Fence new enqueues before taking the lock so concurrent workers
     * stop immediately; they no longer compete for the lock during
     * teardown. */
    atomic_store_explicit(&g_batch_ctx.shutting_down, 1, memory_order_release);

    uv_mutex_lock(&g_batch_ctx.lock);
    if (!g_batch_ctx.initialized) {
        uv_mutex_unlock(&g_batch_ctx.lock);
        return;
    }

    g_batch_ctx.closing = true;
    uv_timer_stop(&g_batch_ctx.flush_timer);
    uv_close((uv_handle_t *)&g_batch_ctx.flush_timer, on_batch_close);

    async_batch_item_t *item;
    while ((item = queue_pop_locked()) != NULL) {
        free(item->query);
        free(item);
    }
    uv_mutex_unlock(&g_batch_ctx.lock);
}

int spine_async_batch_get_stats(spine_async_batch_stats_t *out_stats) {
    if (out_stats == NULL) return -EINVAL;

    /* Queue-size fields require the lock; running totals are atomic and
     * readable without locking so telemetry polling does not contend
     * with the enqueue hot path. */
    uv_mutex_lock(&g_batch_ctx.lock);
    out_stats->pending_count  = g_batch_ctx.pending_count;
    out_stats->active_queries = g_batch_ctx.active_queries;
    out_stats->max_pending    = g_batch_ctx.max_pending;
    uv_mutex_unlock(&g_batch_ctx.lock);

    out_stats->dropped_queries =
        atomic_load_explicit(&g_batch_ctx.dropped_queries, memory_order_relaxed);
    out_stats->enqueue_failures =
        atomic_load_explicit(&g_batch_ctx.enqueue_failures, memory_order_relaxed);
    out_stats->submitted_queries =
        atomic_load_explicit(&g_batch_ctx.submitted_queries, memory_order_relaxed);
    return 0;
}
