#ifndef SPINE_SNMP_SESSION_FACTORY_H
#define SPINE_SNMP_SESSION_FACTORY_H

#include "common.h"
#include "spine.h"

void *spine_snmp_session_create(const spine_snmp_profile_t *profile);
void spine_snmp_session_destroy(void *sessp);

#endif
