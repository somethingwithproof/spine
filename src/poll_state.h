#ifndef SPINE_POLL_STATE_H
#define SPINE_POLL_STATE_H

#include "spine.h"

typedef enum {
    POLL_STATE_INIT,
    POLL_STATE_PING,
    POLL_STATE_REINDEX,
    POLL_STATE_SNMP,
    POLL_STATE_SCRIPTS,
    POLL_STATE_FLUSH,
    POLL_STATE_DONE
} poll_state_t;

typedef struct {
    poll_state_t state;
    spine_host_t *host;
    int device_counter;
    int host_thread;
    int host_threads;
    int host_data_ids;
    char host_time[SMALL_BUFSIZE];
    double host_time_double;
    int host_errors;
    
    // Internal iteration state
    int current_item_idx;
    target_t *poller_items;
    int num_items;
} poll_context_t;

void spine_async_poll_start(poller_thread_t *det);

#endif
