#ifndef SPINE_COMPOSITION_ROOT_H
#define SPINE_COMPOSITION_ROOT_H

#include "common.h"
#include "spine.h"

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

typedef struct spine_services {
	db_ops_t db;
	snmp_ops_t snmp;
	script_ops_t script;
	ping_ops_t ping;
	logger_ops_t logger;
} spine_services_t;

const spine_services_t *spine_services_default(void);
void spine_services_initialize(void);

#endif
