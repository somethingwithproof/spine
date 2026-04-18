#include "common.h"
#include "spine.h"
#include "async_mysql.h"

typedef struct {
    uv_poll_t poll;
    MYSQL *mysql;
    async_mysql_cb callback;
    void *data;
    int closed_handles;
} async_mysql_ctx_t;

static void on_mysql_close(uv_handle_t *handle) {
    async_mysql_ctx_t *ctx = (async_mysql_ctx_t *)handle->data;
    ctx->closed_handles++;
    if (ctx->closed_handles == 1) {
        free(ctx);
    }
}

static void mysql_poll_cb(uv_poll_t* handle, int status, int events) {
    async_mysql_ctx_t *ctx = (async_mysql_ctx_t *)handle->data;
    
    if (status < 0) {
        ctx->callback(ctx->mysql, status, ctx->data);
        uv_close((uv_handle_t*)handle, on_mysql_close);
        return;
    }

    int mysql_status = 0;
    if (events & UV_READABLE) mysql_status |= MYSQL_WAIT_READ;
    if (events & UV_WRITABLE) mysql_status |= MYSQL_WAIT_WRITE;

    // Continue the query
    int next_status = mysql_real_query_cont(&mysql_status, ctx->mysql, mysql_status);

    if (next_status == 0) {
        // Query complete
        ctx->callback(ctx->mysql, 0, ctx->data);
        uv_close((uv_handle_t*)handle, on_mysql_close);
    } else {
        // Still waiting, update poll if needed
        int new_events = 0;
        if (next_status & MYSQL_WAIT_READ) new_events |= UV_READABLE;
        if (next_status & MYSQL_WAIT_WRITE) new_events |= UV_WRITABLE;
        uv_poll_start(handle, new_events, mysql_poll_cb);
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

    int status = mysql_real_query_start(&status, mysql, query, strlen(query));

    if (status == 0) {
        // Finished immediately
        cb(mysql, 0, data);
        free(ctx);
        return 0;
    }

    int events = 0;
    if (status & MYSQL_WAIT_READ) events |= UV_READABLE;
    if (status & MYSQL_WAIT_WRITE) events |= UV_WRITABLE;

    int fd = mysql_get_socket(mysql);
    uv_poll_init(loop, &ctx->poll, fd);
    ctx->poll.data = ctx;
    uv_poll_start(&ctx->poll, events, mysql_poll_cb);

    return 0;
#endif
}
