#include "common.h"
#include "spine.h"
#include "async_dns.h"

/*
 * Async hostname resolution via libuv's uv_getaddrinfo, which runs
 * getaddrinfo(3) on the libuv worker threadpool. For large host counts,
 * raise UV_THREADPOOL_SIZE in the environment; the default of four
 * worker threads bottlenecks scale.
 *
 * An earlier version linked c-ares but never wired its fds to libuv
 * (no uv_poll_t, no ares_process_fd, no timer from ares_timeout),
 * so every resolution leaked the ctx and callbacks never fired. The
 * c-ares bridge was removed rather than shipped half-written; wiring
 * it correctly is tracked as a follow-up.
 */

typedef struct {
    uv_getaddrinfo_t req;
    async_dns_cb     callback;
    void            *data;
} async_dns_ctx_t;

static void on_resolved(uv_getaddrinfo_t *req, int status, struct addrinfo *res) {
    async_dns_ctx_t *ctx = (async_dns_ctx_t *)req->data;

    if (status == 0) {
        ctx->callback(res, 0, ctx->data);
        uv_freeaddrinfo(res);
    } else {
        ctx->callback(NULL, status, ctx->data);
    }

    free(ctx);
}

int spine_async_dns_lookup(const char *hostname, async_dns_cb cb, void *data) {
    if (!hostname || !cb) return -EINVAL;

    async_dns_ctx_t *ctx = calloc(1, sizeof(async_dns_ctx_t));
    if (!ctx) return -ENOMEM;

    ctx->callback = cb;
    ctx->data     = data;
    ctx->req.data = ctx;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int r = uv_getaddrinfo(loop, &ctx->req, on_resolved, hostname, NULL, &hints);
    if (r != 0) {
        free(ctx);
    }
    return r;
}
