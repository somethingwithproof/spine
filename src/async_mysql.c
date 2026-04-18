#include "common.h"
#include "spine.h"
#include "async_mysql.h"

#ifdef HAVE_MYSQL_ASYNC

typedef struct {
    MYSQL *mysql;
    async_mysql_cb callback;
    void *data;
    int ret;
    uv_poll_t poll;
    int fd;
} async_mysql_ctx_t;

static void on_mysql_close(uv_handle_t* handle) {
    free(handle->data);
}

static void mysql_poll_cb(uv_poll_t* handle, int status, int events) {
    async_mysql_ctx_t *ctx = (async_mysql_ctx_t *)handle->data;
    (void)status;

    int mysql_status = 0;
    if (events & UV_READABLE) mysql_status |= MYSQL_WAIT_READ;
    if (events & UV_WRITABLE) mysql_status |= MYSQL_WAIT_WRITE;

    int next_mask = mysql_real_query_cont(&ctx->ret, ctx->mysql, mysql_status);

    if (next_mask == 0) {
        uv_poll_stop(handle);
        ctx->callback(ctx->mysql, ctx->ret, ctx->data);
        uv_close((uv_handle_t*)handle, on_mysql_close);
    } else {
        int new_uv_events = 0;
        if (next_mask & MYSQL_WAIT_READ) new_uv_events |= UV_READABLE;
        if (next_mask & MYSQL_WAIT_WRITE) new_uv_events |= UV_WRITABLE;
        uv_poll_start(handle, new_uv_events, mysql_poll_cb);
    }
}

int spine_async_mysql_query(MYSQL *mysql, const char *query, async_mysql_cb cb, void *data) {
    if (!mysql) return -1;

    async_mysql_ctx_t *ctx = calloc(1, sizeof(async_mysql_ctx_t));
    ctx->mysql = mysql;
    ctx->callback = cb;
    ctx->data = data;
    ctx->fd = mysql_get_socket(mysql);

    int status = mysql_real_query_start(&ctx->ret, mysql, query, strlen(query));

    if (status == 0) {
        cb(mysql, ctx->ret, data);
        free(ctx);
    } else {
        uv_poll_init(loop, &ctx->poll, ctx->fd);
        ctx->poll.data = ctx;
        int new_uv_events = 0;
        if (status & MYSQL_WAIT_READ) new_uv_events |= UV_READABLE;
        if (status & MYSQL_WAIT_WRITE) new_uv_events |= UV_WRITABLE;
        uv_poll_start(&ctx->poll, new_uv_events, mysql_poll_cb);
    }

    return 0;
}

#else

int spine_async_mysql_query(MYSQL *mysql, const char *query, async_mysql_cb cb, void *data) {
    // Fallback for non-async MySQL
    int ret = mysql_real_query(mysql, query, strlen(query));
    cb(mysql, ret, data);
    return 0;
}

#endif
