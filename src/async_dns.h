#ifndef SPINE_ASYNC_DNS_H
#define SPINE_ASYNC_DNS_H

#include <uv.h>

typedef void (*async_dns_cb)(struct addrinfo *res, int status, void *data);

int spine_async_dns_lookup(const char *hostname, async_dns_cb cb, void *data);

#endif
