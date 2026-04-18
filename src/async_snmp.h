#ifndef SPINE_ASYNC_SNMP_H
#define SPINE_ASYNC_SNMP_H

#include <uv.h>
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>

typedef void (*async_snmp_cb)(void *sessp, struct snmp_pdu *pdu, void *data);

int spine_async_snmp_get(void *sessp, const char *oid_str, async_snmp_cb cb, void *data);

#endif
