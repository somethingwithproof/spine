#include "common.h"
#include "spine.h"
#include "snmp_session_factory.h"

void *spine_snmp_session_create(const spine_snmp_profile_t *profile) {
	return snmp_host_init(profile);
}

void spine_snmp_session_destroy(void *sessp) {
	snmp_host_cleanup(sessp);
}
