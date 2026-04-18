#include "common.h"
#include "spine.h"
#include "poll_state.h"
#include "async_snmp.h"

static void on_snmp_readable(uv_poll_t *handle, int status, int events);
static void on_snmp_timeout(uv_timer_t *handle);

/**
 * spine_sync_snmp_to_uv - Project net-snmp state onto libuv handles.
 */
static void spine_sync_snmp_to_uv(poll_context_t *ctx) {
    if (!ctx || !ctx->sessp) {
        return;
    }

    int max_fd = 0;
    int block = 1;
    struct timeval timeout = {0};
    fd_set read_fds;

    FD_ZERO(&read_fds);

    /* Extract state strictly for this target's session.
     * Note: using the older select_info if select_info2 is unavailable,
     * but most modern net-snmp has the session-specific variant. */
    snmp_sess_select_info(ctx->sessp, &max_fd, &read_fds, &timeout, &block);

    /* 1. Timer Management */
    if (block == 0) {
        uint64_t timeout_ms = (timeout.tv_sec * 1000) + (timeout.tv_usec / 1000);
        if (timeout_ms == 0) timeout_ms = 1; /* Yield execution */
        
        uv_timer_start(&ctx->snmp_timer, on_snmp_timeout, timeout_ms, 0);
    } else {
        uv_timer_stop(&ctx->snmp_timer);
    }

    /* 2. File Descriptor Extraction and libuv Mapping */
    int found_fd = -1;
    for (int i = 0; i < max_fd; i++) {
        if (FD_ISSET(i, &read_fds)) {
            found_fd = i;
            break; /* SNMP typically uses 1 UDP socket per session */
        }
    }

    if (found_fd != -1) {
        /* State transition: New socket detected */
        if (ctx->active_fd != found_fd) {
            if (ctx->active_fd != -1) {
                uv_poll_stop(&ctx->snmp_poll);
            }
            
            ctx->active_fd = found_fd;
            uv_poll_init(loop, &ctx->snmp_poll, ctx->active_fd);
            ctx->snmp_poll.data = ctx;
            uv_poll_start(&ctx->snmp_poll, UV_READABLE, on_snmp_readable);
        }
    } else {
        /* State transition: No active sockets (request complete or failed) */
        if (ctx->active_fd != -1) {
            uv_poll_stop(&ctx->snmp_poll);
            ctx->active_fd = -1;
        }
    }
}

static void on_snmp_readable(uv_poll_t *handle, int status, int events) {
    poll_context_t *ctx = (poll_context_t *)handle->data;

    if (status < 0) {
        ctx->state = POLL_STATE_ERROR;
        spine_transition_state(ctx);
        return;
    }

    if (events & UV_READABLE) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(ctx->active_fd, &read_fds);

        /* Hand the raw socket data to net-snmp for parsing.
           This will internally trigger your async_response_handler. */
        snmp_sess_read(ctx->sessp, &read_fds);

        /* Resynchronize handles based on the new internal net-snmp state */
        spine_sync_snmp_to_uv(ctx);
    }
}

static void on_snmp_timeout(uv_timer_t *handle) {
    poll_context_t *ctx = (poll_context_t *)handle->data;

    /* net-snmp handles the internal retry counter.
       If it exhausts retries, it triggers the callback with NETSNMP_CALLBACK_OP_TIMED_OUT */
    snmp_sess_timeout(ctx->sessp);
    
    spine_sync_snmp_to_uv(ctx);
}

// Forward declaration of extraction helper
extern void extract_and_format_pdu(struct snmp_pdu *pdu, poll_context_t *ctx);

/* Internal Net-SNMP response callback */
static int async_response_handler(int operation, struct snmp_session *sp, 
                                  int reqid, struct snmp_pdu *pdu, void *magic) {
    (void)sp;
    (void)reqid;
    poll_context_t *ctx = (poll_context_t *)magic;

    if (operation == NETSNMP_CALLBACK_OP_RECEIVED_MESSAGE) {
        /* 1. Extract VarBinds and format data */
        extract_and_format_pdu(pdu, ctx);
        
        /* 2. Transition state explicitly */
        ctx->state = POLL_STATE_SCRIPTS;
        spine_transition_state(ctx);
        
    } else if (operation == NETSNMP_CALLBACK_OP_TIMED_OUT) {
        ctx->state = POLL_STATE_ERROR;
        spine_transition_state(ctx);
    }
    
    return 1; /* Acknowledge we handled the PDU */
}

int spine_async_snmp_get(poll_context_t *ctx, const char *oid_str) {
    struct snmp_pdu *pdu;
    oid anOID[MAX_OID_LEN];
    size_t anOID_len = MAX_OID_LEN;

    if (!snmp_parse_oid(oid_str, anOID, &anOID_len)) {
        return -1;
    }

    pdu = snmp_pdu_create(SNMP_MSG_GET);
    snmp_add_null_var(pdu, anOID, anOID_len);

    /* Send the async request. Callback is registered with the session? 
     * Actually, Net-SNMP sess_async_send takes the callback directly. */
    if (snmp_sess_async_send(ctx->sessp, pdu, async_response_handler, ctx) == 0) {
        snmp_free_pdu(pdu);
        return -1;
    }

    /* Resynchronize to ensure libuv starts polling for the outgoing request */
    spine_sync_snmp_to_uv(ctx);

    return 0;
}
