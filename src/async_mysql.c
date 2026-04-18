#include "common.h"
#include "spine.h"
#include "async_mysql.h"

typedef struct {
    uv_poll_t poll;
    MYSQL *mysql;
    async_mysql_cb callback;
    void *data;
    int ret; // Receives the MySQL return code (0=success)
} async_mysql_ctx_t;

static void on_mysql_close(uv_handle_t *handle) {
    async_mysql_ctx_t *ctx = (async_mysql_ctx_t *)handle->data;
    free(ctx);
}

static void mysql_poll_cb(uv_poll_t* handle, int status, int events) {
    async_mysql_ctx_t *ctx = (async_mysql_ctx_t *)handle->data;
    
    if (status < 0) {
        ctx->callback(ctx->mysql, status, ctx->data);
        uv_close((uv_handle_t*)handle, on_mysql_close);
        return;
    }

    // Map libuv events back to MariaDB status flags
    int mysql_status = 0;
    if (events & UV_READABLE) mysql_status |= MYSQL_WAIT_READ;
    if (events & UV_WRITABLE) mysql_status |= MYSQL_WAIT_WRITE;

    // Continue the query. next_mask tells us what to wait for next.
    int next_mask = mysql_real_query_cont(&ctx->ret, ctx->mysql, mysql_status);

    if (next_mask == 0) {
        // Query fully complete. Use the captured 'ret' for status.
        ctx->callback(ctx->mysql, ctx->ret, ctx->data);
        uv_close((uv_handle_t*)handle, on_mysql_close);
    } else {
        // More I/O required, update poll events
        int new_uv_events = 0;
        if (next_mask & MYSQL_WAIT_READ) new_uv_events |= UV_READABLE;
        if (next_mask & MYSQL_WAIT_WRITE) new_uv_events |= UV_WRITABLE;
        
        uv_poll_start(handle, new_uv_events, mysql_poll_cb);
    }
}

int spine_async_mysql_query(MYSQL *mysql, const char *query, async_mysql_cb cb, void *data) {
#ifndef HAVE_MYSQL_ASYNC
    (void)mysql; (void)query; (void)cb; (void)data;
    return -1;
#else
    async_mysql_ctx_t *ctx = calloc(1, sizeof(async_mysql_ctx_t));
    ctx->mysql = mysql;
    ctx->callback = cb;
    ctx->data = data;

    // Start the non-blocking query
    int next_mask = mysql_real_query_start(&ctx->ret, mysql, query, strlen(query));

    if (next_mask == 0) {
        // Finished immediately without needing to wait for I/O
        cb(mysql, ctx->ret, data);
        free(ctx);
        return 0;
    }

    int uv_events = 0;
    if (next_mask & MYSQL_WAIT_READ) uv_events |= UV_READABLE;
    if (next_mask & MYSQL_WAIT_WRITE) uv_events |= UV_WRITABLE;

    int fd = mysql_get_socket(mysql);
    uv_poll_init(loop, &ctx->poll, fd);
    ctx->poll.data = ctx;
    uv_poll_start(&ctx->poll, uv_events, mysql_poll_cb);

    return 0;
#endif
}
