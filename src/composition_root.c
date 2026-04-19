#include "common.h"
#include "spine.h"
#include "composition_root.h"

typedef struct db_ops {
	pool_t *(*get_connection)(int type);
	void (*release_connection)(int type, int id);
	MYSQL_RES *(*query)(MYSQL *mysql, int type, const char *query);
	int (*insert)(MYSQL *mysql, int type, const char *query);
	void (*escape)(MYSQL *mysql, char *to, int to_len, const char *from);
} db_ops_t;

typedef struct snmp_ops {
	void *(*host_init)(int host_id, char *hostname, int snmp_version, char *snmp_community,
		char *snmp_username, char *snmp_password, char *snmp_auth_protocol,
		char *snmp_priv_passphrase, char *snmp_priv_protocol,
		char *snmp_context, char *snmp_engine_id, int snmp_port, int snmp_timeout);
	void (*host_cleanup)(void *sessp);
	char *(*get)(spine_spine_host_t *host, const char *oid);
	char *(*get_base)(spine_spine_host_t *host, const char *oid, bool numeric);
} snmp_ops_t;

typedef struct script_ops {
	char *(*exec_poll)(int host_id, char *command, int id);
} script_ops_t;

typedef struct ping_ops {
	int (*ping_host)(spine_spine_host_t *host, ping_t *ping);
	void (*update_host_status)(int status, spine_spine_host_t *host, ping_t *ping, int availability_method);
} ping_ops_t;

typedef struct logger_ops {
	int (*log)(const char *format, ...);
} logger_ops_t;

struct spine_services {
	db_ops_t db;
	snmp_ops_t snmp;
	script_ops_t script;
	ping_ops_t ping;
	logger_ops_t logger;
};

static char *script_exec_poll_adapter(int host_id, char *command, int id) {
	spine_spine_host_t host;
	memset(&host, 0, sizeof(host));
	host.id = host_id;
	return exec_poll(&host, command, id, "DS");
}

const spine_services_t *spine_services_default(void) {
	static const struct spine_services services = {
		.db = {
			.get_connection = db_get_connection,
			.release_connection = db_release_connection,
			.query = db_query,
			.insert = db_insert,
			.escape = db_escape
		},
		.snmp = {
			.host_init = snmp_host_init,
			.host_cleanup = snmp_host_cleanup,
			.get = snmp_get,
			.get_base = snmp_get_base
		},
		.script = {
			.exec_poll = script_exec_poll_adapter
		},
		.ping = {
			.ping_host = ping_host,
			.update_host_status = update_host_status
		},
		.logger = {
			.log = spine_log
		}
	};

	return &services;
}

void spine_services_initialize(void) {
	(void)spine_services_default();
}
