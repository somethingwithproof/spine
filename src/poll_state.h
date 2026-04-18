#ifndef SPINE_POLL_STATE_H
#define SPINE_POLL_STATE_H

#include "common.h"
#include "spine.h"
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>

typedef enum {
    POLL_STATE_INIT,
    POLL_STATE_DNS,
    POLL_STATE_PING,
    POLL_STATE_REINDEX,
    POLL_STATE_SNMP_SEND,
    POLL_STATE_SNMP_WAIT,
    POLL_STATE_SCRIPTS,
    POLL_STATE_FLUSH,
    POLL_STATE_DONE,
    POLL_STATE_ERROR
} poll_state_t;

typedef struct {
    poll_state_t state;
    spine_spine_host_t *host;
    int device_counter;
    int spine_host_thread;
    int spine_host_threads;
    int host_data_ids;
    char spine_host_time[SMALL_BUFSIZE];
    double host_time_double;
    int host_errors;
    
    /* Net-SNMP and libuv bridge state */
    uv_poll_t snmp_poll;
    uv_timer_t snmp_timer;
    void *sessp;          /* Opaque net-snmp thread-safe session */
    int active_fd;        /* Cached file descriptor for diffing */
    int handles_closed;   /* Tracker for uv_close synchronization */

    /* Internal iteration state */
    int current_item_idx;
    target_t *poller_items;
    int num_items;
} poll_context_t;

void spine_async_poll_start(poller_thread_t *det);
void spine_transition_state(poll_context_t *ctx);

#endif
