/*
 * Standalone ICMP echo payload validator.
 *
 * Extracted into its own translation unit so unit tests can link just
 * this object without pulling in the full spine runtime (mysql,
 * net-snmp, the poller, etc). Wire layout is defined once in
 * ping_wire.h; this TU implements the validation contract declared
 * there.
 */
#include "ping_wire.h"
#include <string.h>

int spine_ping_validate_payload(const void *buf, size_t len,
                                uint32_t expect_pid_mask) {
  spine_ping_payload_t payload;
  if (buf == NULL) {
    return 0;
	}
	if (len < sizeof(spine_ping_payload_t)) {
		return 0;
	}
        /* Packet buffers need not have the alignment required by this struct.
         */
        memcpy(&payload, buf, sizeof(payload));
        if (payload.magic != SPINE_PING_MAGIC) {
          return 0;
        }
        if (payload.pid_mask != expect_pid_mask) {
          return 0;
        }
        return 1;
}
