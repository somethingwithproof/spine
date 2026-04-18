#include "common.h"
#include "spine.h"
#include "async_icmp.h"

int spine_async_icmp_ping(const char *hostname, async_icmp_cb cb, void *data) {
    (void)hostname;
    // (Stub: In Phase 5 we'll integrate with src/ping.c's raw socket)
    // For now, fire immediate callback to allow state machine testing.
    cb("UP", 0, data);

    return 0;
}
