#ifndef SPINE_ASYNC_SNMP_H
#define SPINE_ASYNC_SNMP_H

#include "poll_state.h"

int spine_async_snmp_get(poll_context_t *ctx, const char *oid_str);

#endif
