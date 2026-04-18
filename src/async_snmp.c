#include "common.h"
#include "spine.h"
#include "async_snmp.h"

typedef struct {
    uv_poll_t poll;
    void *sessp;
    async_snmp_cb callback;
    void *data;
    int closed_handles;
    int fd;
} async_snmp_ctx_t;

static void on_poll_close(uv_handle_t *handle) {
    async_snmp_ctx_t *ctx = (async_snmp_ctx_t *)handle->data;
    ctx->closed_handles++;
    if (ctx->closed_handles == 1) {
        free(ctx);
    }
}

static void snmp_poll_cb(uv_poll_t* handle, int status, int events) {
    async_snmp_ctx_t *ctx = (async_snmp_ctx_t *)handle->data;
    
    if (status < 0) {
        ctx->callback(ctx->sessp, NULL, ctx->data);
        uv_close((uv_handle_t*)handle, on_poll_close);
        return;
    }

    fd_set fdset;
    FD_ZERO(&fdset);
    if (events & UV_READABLE) FD_SET(ctx->fd, &fdset);
    
    // Tell Net-SNMP to process the ready FD
    snmp_sess_read(ctx->sessp, &fdset);
}

// Internal callback for Net-SNMP
static int snmp_response_cb(int operation, struct snmp_session *sp, int reqid, struct snmp_pdu *pdu, void *magic) {
    (void)operation;
    (void)sp;
    (void)reqid;
    async_snmp_ctx_t *ctx = (async_snmp_ctx_t *)magic;
    
    ctx->callback(ctx->sessp, pdu, ctx->data);
    
    uv_close((uv_handle_t*)&ctx->poll, on_poll_close);
    
    return 1;
}

int spine_async_snmp_get(void *sessp, const char *oid_str, async_snmp_cb cb, void *data) {
    struct snmp_pdu *pdu;
    oid anOID[MAX_OID_LEN];
    size_t anOID_len = MAX_OID_LEN;

    if (!snmp_parse_oid(oid_str, anOID, &anOID_len)) {
        return -1;
    }

    pdu = snmp_pdu_create(SNMP_MSG_GET);
    snmp_add_null_var(pdu, anOID, anOID_len);

    async_snmp_ctx_t *ctx = calloc(1, sizeof(async_snmp_ctx_t));
    ctx->sessp = sessp;
    ctx->callback = cb;
    ctx->data = data;

    // Send the async request
    if (snmp_sess_async_send(sessp, pdu, snmp_response_cb, ctx) == 0) {
        snmp_free_pdu(pdu);
        free(ctx);
        return -1;
    }

    // Now we must find the FD and register it with libuv
    int fds = 0, block = 1;
    fd_set fdset;
    struct timeval timeout;
    FD_ZERO(&fdset);
    snmp_sess_select_info(sessp, &fds, &fdset, &timeout, &block);

    // Find which FD was set
    int fd_found = 0;
    for (int i = 0; i < fds; i++) {
        if (FD_ISSET(i, &fdset)) {
            ctx->fd = i;
            uv_poll_init(loop, &ctx->poll, i);
            ctx->poll.data = ctx;
            uv_poll_start(&ctx->poll, UV_READABLE, snmp_poll_cb);
            fd_found = 1;
            break;
        }
    }

    if (!fd_found) {
        free(ctx);
        return -1;
    }

    return 0;
}
