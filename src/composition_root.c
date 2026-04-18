#include "common.h"
#include "spine.h"
#include "composition_root.h"

static char *script_exec_poll_adapter(int host_id, char *command, int id) {
	spine_spine_host_t host;
	memset(&host, 0, sizeof(host));
	host.id = host_id;
	return exec_poll(&host, command, id, "DS");
}

const spine_services_t *spine_services_default(void) {
	static spine_services_t services = {0};

	services.db.get_connection = db_get_connection;
	services.db.release_connection = db_release_connection;
	services.db.query = db_query;
	services.db.insert = db_insert;
	services.db.escape = db_escape;

	services.snmp.host_init = snmp_host_init;
	services.snmp.host_cleanup = snmp_host_cleanup;
	services.snmp.get = snmp_get;
	services.snmp.get_base = snmp_get_base;

	services.script.exec_poll = script_exec_poll_adapter;

	services.ping.ping_host = ping_host;
	services.ping.update_host_status = update_host_status;

	services.logger.log = spine_log;

	return &services;
}

void spine_services_initialize(void) {
	(void)spine_services_default();
}
