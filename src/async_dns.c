#include "common.h"
#include "spine.h"
#include "async_dns.h"

#ifdef HAVE_CARES
#include <ares.h>

typedef struct {
    async_dns_cb callback;
    void *data;
} async_dns_ctx_t;

static void on_ares_resolved(void *arg, int status, int timeouts, struct hostent *host) {
    async_dns_ctx_t *ctx = (async_dns_ctx_t *)arg;
    (void)timeouts;

    if (status == ARES_SUCCESS && host) {
        ctx->callback(NULL, 0, ctx->data);
    } else {
        ctx->callback(NULL, status, ctx->data);
    }

    free(ctx);
}

static void on_ares_poll(uv_poll_t* handle, int status, int events) {
    ares_channel channel = (ares_channel)handle->data;
    (void)status;

    uv_os_fd_t fd;
    uv_fileno((uv_handle_t*)handle, &fd);

    ares_process_fd(channel, 
        (events & UV_READABLE) ? (ares_socket_t)fd : ARES_SOCKET_BAD,
        (events & UV_WRITABLE) ? (ares_socket_t)fd : ARES_SOCKET_BAD);
}

int spine_async_dns_lookup(const char *hostname, async_dns_cb cb, void *data) {
    static ares_channel channel;
    static bool ares_is_init = false;

    if (!ares_is_init) {
        if (ares_library_init(ARES_LIB_INIT_ALL) != 0) return -1;
        if (ares_init(&channel) != ARES_SUCCESS) return -1;
        ares_is_init = true;
    }

    async_dns_ctx_t *ctx = calloc(1, sizeof(async_dns_ctx_t));
    ctx->callback = cb;
    ctx->data = data;

    // Use non-deprecated API if possible, but for this bridge we'll stick to basic host lookup
    ares_gethostbyname(channel, hostname, AF_INET, on_ares_resolved, ctx);
    
    return 0;
}
#else
// uv_getaddrinfo fallback
typedef struct {
    uv_getaddrinfo_t req;
    async_dns_cb callback;
    void *data;
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
    async_dns_ctx_t *ctx = calloc(1, sizeof(async_dns_ctx_t));
    if (!ctx) return -ENOMEM;
    ctx->callback = cb;
    ctx->data = data;
    ctx->req.data = ctx;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    return uv_getaddrinfo(loop, &ctx->req, on_resolved, hostname, NULL, &hints);
}
#endif
