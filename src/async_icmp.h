#ifndef SPINE_ASYNC_ICMP_H
#define SPINE_ASYNC_ICMP_H

#include <uv.h>

typedef void (*async_icmp_cb)(const char *result, int status, void *data);

int spine_async_icmp_ping(const char *hostname, async_icmp_cb cb, void *data);

#endif
