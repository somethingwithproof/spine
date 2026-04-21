#include "common.h"
#include "spine.h"
#include "async_mysql.h"
#include <errno.h>

#ifdef HAVE_MYSQL_ASYNC

typedef struct {
    MYSQL *mysql;
    async_mysql_cb callback;
    void *data;
    int ret;
    uv_poll_t poll;
    int fd;
} async_mysql_ctx_t;

typedef struct async_mysql_active {
    MYSQL *mysql;
    struct async_mysql_active *next;
} async_mysql_active_t;

static async_mysql_active_t *g_active_mysql = NULL;

/* Main-loop-thread assertion. g_active_mysql is an intrusive linked
 * list mutated by mark/unmark on submit and close-callback paths; both
 * must run on the libuv event-loop thread. If a future refactor calls
 * spine_async_mysql_query from a worker thread, the list mutations
 * would race the close-callback chain and silently corrupt the list.
 *
 * The owner thread is seeded once at init time by
 * spine_async_mysql_bind_main_thread() BEFORE any worker threads
 * exist, so the assertion never races the first write. If
 * bind_main_thread is not called (unit-test path), the first caller
 * seeds itself under a one-way atomic latch; subsequent first-call
 * races from multiple threads can fire a spurious die() in that case,
 * which is still preferable to silent list corruption. */
static uv_thread_t g_async_mysql_owner_thread;
static atomic_int g_async_mysql_owner_set = 0;

void spine_async_mysql_bind_main_thread(void) {
    /* Idempotent: rebinding to the same thread is a no-op; rebinding
     * from a different thread die()s because that is the same kind of
     * invariant violation the assertion exists to catch. */
    uv_thread_t self = uv_thread_self();
    int was_set = atomic_exchange_explicit(&g_async_mysql_owner_set, 1,
                                           memory_order_acq_rel);
    if (!was_set) {
        g_async_mysql_owner_thread = self;
        return;
    }
    if (!uv_thread_equal(&g_async_mysql_owner_thread, &self)) {
        die("FATAL: spine_async_mysql_bind_main_thread called twice "
            "from different threads");
    }
}

static void async_mysql_assert_owner_thread(void) {
    uv_thread_t self = uv_thread_self();
    int was_set = atomic_exchange_explicit(&g_async_mysql_owner_set, 1,
                                           memory_order_acq_rel);
    if (!was_set) {
        /* Late-bind fallback for tests that do not call the public
         * binder. See header comment above. */
        g_async_mysql_owner_thread = self;
        return;
    }
    if (!uv_thread_equal(&g_async_mysql_owner_thread, &self)) {
        die("FATAL: async_mysql invoked from non-owner thread; "
            "g_active_mysql is main-loop-only");
    }
}

/* Shutdown fence. Once set, new async queries are refused so the MYSQL
 * handles can be safely torn down without a late callback dereferencing
 * freed state. In-flight queries continue through their existing
 * uv_poll callback chain; the uv_run drain flushes them.
 *
 * Declared _Atomic with release/acquire ordering so worker threads on
 * weakly-ordered architectures (ARM, POWER) see the flag promptly; a
 * plain volatile int is not a sufficient memory barrier across thread
 * boundaries. */
#include <stdatomic.h>
static atomic_int g_async_mysql_shutting_down = 0;

/* Count of submissions refused after the shutdown fence flipped. Read
 * via spine_async_mysql_shutdown_refused_count(); operators use this to
 * detect worker-thread coordination lag at shutdown. */
static atomic_ulong g_async_mysql_refused_after_shutdown = 0;

void spine_async_mysql_shutdown_begin(void) {
    atomic_store_explicit(&g_async_mysql_shutting_down, 1,
                          memory_order_release);
}

unsigned long spine_async_mysql_shutdown_refused_count(void) {
    return atomic_load_explicit(&g_async_mysql_refused_after_shutdown,
                                memory_order_relaxed);
}

#ifdef SPINE_ENABLE_TEST_HOOKS
/* Test-only: clear the shutdown fence and the refused counter so unit
 * tests can drive the fence more than once. Production code must not
 * call this - the production fence is a one-way latch. Gated behind
 * SPINE_ENABLE_TEST_HOOKS so a production build cannot accidentally
 * link a symbol that resets shutdown state. */
void spine_async_mysql_shutdown_reset_for_test(void) {
    atomic_store_explicit(&g_async_mysql_shutting_down, 0,
                          memory_order_release);
    atomic_store_explicit(&g_async_mysql_refused_after_shutdown, 0,
                          memory_order_relaxed);
}
#endif

static int async_mysql_mark_active(MYSQL *mysql) {
    async_mysql_active_t *node;
    async_mysql_active_t *cursor;

    async_mysql_assert_owner_thread();

    cursor = g_active_mysql;
    while (cursor != NULL) {
        if (cursor->mysql == mysql) {
            return -EALREADY;
        }
        cursor = cursor->next;
    }

    node = (async_mysql_active_t *)calloc(1, sizeof(*node));
    if (node == NULL) {
        return -ENOMEM;
    }
    node->mysql = mysql;
    node->next = g_active_mysql;
    g_active_mysql = node;
    return 0;
}

static void async_mysql_unmark_active(MYSQL *mysql) {
    async_mysql_active_t **cursor;

    async_mysql_assert_owner_thread();

    cursor = &g_active_mysql;
    while (*cursor != NULL) {
        async_mysql_active_t *node = *cursor;
        if (node->mysql == mysql) {
            *cursor = node->next;
            free(node);
            return;
        }
        cursor = &node->next;
    }
}

static void on_mysql_close(uv_handle_t* handle) {
    async_mysql_ctx_t *ctx = (async_mysql_ctx_t *)handle->data;
    if (ctx != NULL) {
        async_mysql_unmark_active(ctx->mysql);
    }
    free(handle->data);
}

static void mysql_poll_cb(uv_poll_t* handle, int status, int events) {
    async_mysql_ctx_t *ctx = (async_mysql_ctx_t *)handle->data;
    if (ctx == NULL) {
        return;
    }

    if (status < 0) {
        uv_poll_stop(handle);
        MYSQL *mysql = ctx->mysql;
        async_mysql_unmark_active(mysql);
        ctx->mysql = NULL;
        ctx->callback(mysql, -EIO, ctx->data);
        uv_close((uv_handle_t*)handle, on_mysql_close);
        return;
    }

    int mysql_status = 0;
    if (events & UV_READABLE) mysql_status |= MYSQL_WAIT_READ;
    if (events & UV_WRITABLE) mysql_status |= MYSQL_WAIT_WRITE;

    int next_mask = mysql_real_query_cont(&ctx->ret, ctx->mysql, mysql_status);

    if (next_mask == 0) {
        uv_poll_stop(handle);
        MYSQL *mysql = ctx->mysql;
        async_mysql_unmark_active(mysql);
        ctx->mysql = NULL;
        ctx->callback(mysql, ctx->ret, ctx->data);
        uv_close((uv_handle_t*)handle, on_mysql_close);
    } else {
        int new_uv_events = 0;
        if (next_mask & MYSQL_WAIT_READ) new_uv_events |= UV_READABLE;
        if (next_mask & MYSQL_WAIT_WRITE) new_uv_events |= UV_WRITABLE;
        uv_poll_start(handle, new_uv_events, mysql_poll_cb);
    }
}

int spine_async_mysql_query(uv_loop_t *runtime_loop, MYSQL *mysql, const char *query, async_mysql_cb cb, void *data) {
    int mark_rc;

    if (!runtime_loop || !mysql || !query || !cb) {
        return -EINVAL;
    }
    if (atomic_load_explicit(&g_async_mysql_shutting_down,
                             memory_order_acquire)) {
        atomic_fetch_add_explicit(&g_async_mysql_refused_after_shutdown,
                                  1, memory_order_relaxed);
        return -ESHUTDOWN;
    }

    async_mysql_ctx_t *ctx = calloc(1, sizeof(async_mysql_ctx_t));
    if (ctx == NULL) {
        return -ENOMEM;
    }

    mark_rc = async_mysql_mark_active(mysql);
    if (mark_rc != 0) {
        free(ctx);
        return mark_rc;
    }

    ctx->mysql = mysql;
    ctx->callback = cb;
    ctx->data = data;
    ctx->fd = mysql_get_socket(mysql);
    if (ctx->fd < 0) {
        async_mysql_unmark_active(mysql);
        free(ctx);
        return -EINVAL;
    }

    int status = mysql_real_query_start(&ctx->ret, mysql, query, strlen(query));

    if (status == 0) {
        async_mysql_unmark_active(mysql);
        cb(mysql, ctx->ret, data);
        free(ctx);
    } else {
        int rc = uv_poll_init(runtime_loop, &ctx->poll, ctx->fd);
        if (rc != 0) {
            async_mysql_unmark_active(mysql);
            free(ctx);
            mysql_close(mysql);
            return rc;
        }
        ctx->poll.data = ctx;
        int new_uv_events = 0;
        if (status & MYSQL_WAIT_READ) new_uv_events |= UV_READABLE;
        if (status & MYSQL_WAIT_WRITE) new_uv_events |= UV_WRITABLE;
        rc = uv_poll_start(&ctx->poll, new_uv_events, mysql_poll_cb);
        if (rc != 0) {
            ctx->mysql = NULL;
            async_mysql_unmark_active(mysql);
            mysql_close(mysql);
            uv_close((uv_handle_t *)&ctx->poll, on_mysql_close);
            return rc;
        }
    }

    return 0;
}

#else

int spine_async_mysql_query(uv_loop_t *runtime_loop, MYSQL *mysql, const char *query, async_mysql_cb cb, void *data) {
    (void)runtime_loop;
    if (!mysql || !query || !cb) return -EINVAL;
    /* Avoid silently blocking the event loop when async C API support
     * is unavailable in the linked client library. */
    return -ENOTSUP;
}

/* Shutdown fence is a no-op when the async path is compiled out; the
 * symbol exists so spine.c can call it unconditionally. */
void spine_async_mysql_shutdown_begin(void) {
}

void spine_async_mysql_bind_main_thread(void) {
    /* No-op: the fallback path has no g_active_mysql list to guard. */
}

unsigned long spine_async_mysql_shutdown_refused_count(void) {
    return 0;
}

#ifdef SPINE_ENABLE_TEST_HOOKS
void spine_async_mysql_shutdown_reset_for_test(void) {
    /* No-op: the fallback path has no real fence state. */
}
#endif

#endif
