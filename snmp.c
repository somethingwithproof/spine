/*
 ex: set tabstop=4 shiftwidth=4 autoindent:
 +-------------------------------------------------------------------------+
 | Copyright (C) 2004-2026 The Cacti Group                                 |
 |                                                                         |
 | This program is free software; you can redistribute it and/or           |
 | modify it under the terms of the GNU Lesser General Public              |
 | License as published by the Free Software Foundation; either            |
 | version 2.1 of the License, or (at your option) any later version. 	   |
 |                                                                         |
 | This program is distributed in the hope that it will be useful,         |
 | but WITHOUT ANY WARRANTY; without even the implied warranty of          |
 | MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           |
 | GNU Lesser General Public License for more details.                     |
 |                                                                         |
 | You should have received a copy of the GNU Lesser General Public        |
 | License along with this library; if not, write to the Free Software     |
 | Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA           |
 | 02110-1301, USA                                                         |
 |                                                                         |
 +-------------------------------------------------------------------------+
 | spine: a backend data gatherer for cacti                                |
 +-------------------------------------------------------------------------+
 | This poller would not have been possible without:                       |
 |   - Larry Adams (current development and enhancements)                  |
 |   - Rivo Nurges (rrd support, mysql poller cache, misc functions)       |
 |   - RTG (core poller code, pthreads, snmp, autoconf examples)           |
 |   - Brady Alleman/Doug Warner (threading ideas, implementation details) |
 +-------------------------------------------------------------------------+
 | - Cacti - http://www.cacti.net/                                         |
 +-------------------------------------------------------------------------+
*/

#include "common.h"
#include "spine.h"

/* resolve problems in debian */
#ifndef NETSNMP_DS_LIB_DONT_PERSIST_STATE
 #define NETSNMP_DS_LIB_DONT_PERSIST_STATE 32
#endif

#define OIDSIZE(p) (sizeof(p)/sizeof(oid))

/*! \fn void snmp_spine_init()
 *  \brief wrapper function for init_snmp
 *
 *	Initializes snmp for the given application ID
 *
 */
void snmp_spine_init(void) {

/* Only do numeric output */
#ifdef NETSNMP_DS_LIB_PRINT_NUMERIC_ENUM
	netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_PRINT_NUMERIC_ENUM, 1);
#endif

/* Prevent update of the snmpapp.conf file */
#ifdef NETSNMP_DS_LIB_DONT_PERSIST_STATE
	netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_DONT_PERSIST_STATE, 1);
#endif

/* Prevent update of the snmpapp.conf file */
#ifdef NETSNMP_DS_LIB_DISABLE_PERSISTENT_LOAD
	netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_DISABLE_PERSISTENT_LOAD, 1);
#endif

#ifdef NETSNMP_DS_LIB_DONT_PRINT_UNITS
	netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_DONT_PRINT_UNITS, 1);
#endif

setenv("MIBS", "", 1);
netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_QUICK_PRINT, 1);
netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_QUICKE_PRINT, 1);
netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_PRINT_BARE_VALUE, 1);
netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_NUMERIC_TIMETICKS, 1);

/* don't check the range of the OID */
netsnmp_ds_toggle_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_DONT_CHECK_RANGE);

#if defined(VERIFY_PACKAGE_VERSION) && defined(PACKAGE_VERSION)
	/* check that the headers we compiled with match the library we linked with */
	SPINE_LOG_DEBUG(("DEBUG: SNMP Header Version is %s", PACKAGE_VERSION));
	SPINE_LOG_DEBUG(("DEBUG: SNMP Library Version is %s", netsnmp_get_version()));

	if (STRMATCH(PACKAGE_VERSION,netsnmp_get_version())) {
		init_snmp("spine");
	} else {
		/* report the error and quit spine */
		die("ERROR: SNMP Library Version Mismatch (%s vs %s)",PACKAGE_VERSION,netsnmp_get_version());
	}
#else
	SPINE_LOG_DEBUG(("DEBUG: Issues with SNMP Header Version information, assuming old version of Net-SNMP."));
	init_snmp("spine");
#endif
}

/*! \fn void snmp_spine_close()
 *  \brief wrapper function for the snmp_shutdown function
 *
 *	Closes the snmp api for the given application ID
 *
 */
void snmp_spine_close(void) {
	snmp_shutdown("spine");
}

static bool snmp_set_security_keys(struct snmp_session *session, int host_id,
		const char *auth_password, const char *priv_password) {
	char *Apsz = NULL;
	char *Xpsz = NULL;
	// Auth Protocol Setup

	Apsz = strdup(auth_password);
	if (Apsz == NULL) die("ERROR: Fatal malloc error: SNMP authentication passphrase");

	// Privacy Protocol Setup

	Xpsz = strdup(priv_password);
	if (Xpsz == NULL) {
		if (Apsz != NULL) spine_clear_sensitive(Apsz, strlen(Apsz));
		free(Apsz);
		die("ERROR: Fatal malloc error: SNMP privacy passphrase");
	}

	if (Apsz) {
		session->securityAuthKeyLen = USM_AUTH_KU_LEN;
		if (session->securityAuthProto == NULL) {
			/*
			 * get .conf set default
			 */
			const oid *def = get_default_authtype(&session->securityAuthProtoLen);
			session->securityAuthProto = snmp_duplicate_objid(def, session->securityAuthProtoLen);
		}

		if (session->securityAuthProto == NULL) {
			session->securityAuthProto    = snmp_duplicate_objid(SNMP_DEFAULT_AUTH_PROTO, SNMP_DEFAULT_AUTH_PROTOLEN);
			session->securityAuthProtoLen = SNMP_DEFAULT_AUTH_PROTOLEN;
		}

		if (generate_Ku(session->securityAuthProto,
			(u_int)spine_count_to_int(session->securityAuthProtoLen),
			(u_char *) Apsz, strlen(Apsz),
			session->securityAuthKey,
			&session->securityAuthKeyLen) != SNMPERR_SUCCESS) {
			SPINE_LOG(("SNMP: Device[%i] Error generating SNMPv3 Ku from authentication passphrase.", host_id));
			if (Apsz != NULL) spine_clear_sensitive(Apsz, strlen(Apsz));
			free(Apsz);
			spine_clear_sensitive(Xpsz, strlen(Xpsz));
			free(Xpsz);
			return FALSE;
		}

		if (Apsz != NULL) spine_clear_sensitive(Apsz, strlen(Apsz));
		free(Apsz);
		Apsz = NULL;
	}

	if (Xpsz) {
		session->securityPrivKeyLen = USM_PRIV_KU_LEN;
		if (session->securityPrivProto == NULL) {
			/*
			 * get .conf set default
			 */
			const oid *def = get_default_privtype(&session->securityPrivProtoLen);
			session->securityPrivProto =
			snmp_duplicate_objid(def, session->securityPrivProtoLen);
		}

		if (session->securityPrivProto == NULL) {
			session->securityPrivProto = snmp_duplicate_objid(SNMP_DEFAULT_PRIV_PROTO, SNMP_DEFAULT_PRIV_PROTOLEN);
			session->securityPrivProtoLen = SNMP_DEFAULT_PRIV_PROTOLEN;
		}

		if (generate_Ku(session->securityAuthProto,
			(u_int)spine_count_to_int(session->securityAuthProtoLen),
			(u_char *) Xpsz, strlen(Xpsz),
			session->securityPrivKey,
			&session->securityPrivKeyLen) != SNMPERR_SUCCESS) {
			SPINE_LOG(("SNMP: Device[%i] Error generating SNMPv3 Ku from privacy pass phrase.", host_id));
			if (Apsz != NULL) spine_clear_sensitive(Apsz, strlen(Apsz));
			free(Apsz);
			spine_clear_sensitive(Xpsz, strlen(Xpsz));
			free(Xpsz);
			return FALSE;
		}

		spine_clear_sensitive(Xpsz, strlen(Xpsz));
		free(Xpsz);
		Xpsz = NULL;
	}
	return TRUE;
}

/*! \fn void *snmp_host_init(int host_id, char *hostname, int snmp_version,
 * char *snmp_community, char *snmp_username, const char *snmp_password,
 * char *snmp_auth_protocol, const char *snmp_priv_passphrase, char *snmp_priv_protocol,
 * char *snmp_context, char *snmp_engine_id, int snmp_port, int snmp_timeout)
 *  \brief initializes an snmp_session object for a Spine host
 *
 *	This function will initialize NET-SNMP for the Spine host
 *  in question.
 *
 */
static void snmp_host_init_release(struct snmp_session *session, char *auth, char *priv) {
	if (auth != NULL) spine_clear_sensitive(auth, strlen(auth));
	if (priv != NULL) spine_clear_sensitive(priv, strlen(priv));
	free(auth);
	free(priv);
	#if SNMP_LOCALNAME == 1
	free(session->localname);
	#endif
	free(session->securityAuthProto);
	free(session->securityPrivProto);
	spine_clear_sensitive(session->securityAuthKey, sizeof(session->securityAuthKey));
	spine_clear_sensitive(session->securityPrivKey, sizeof(session->securityPrivKey));
}

void *snmp_host_init(int host_id, char *hostname, int snmp_version, char *snmp_community,
	char *snmp_username, const char *snmp_password, char *snmp_auth_protocol,
	const char *snmp_priv_passphrase, char *snmp_priv_protocol,
	char *snmp_context, char *snmp_engine_id, int snmp_port, int snmp_timeout) {

	void   *sessp = NULL;
	struct snmp_session session;
	char   hostnameport[BUFSIZE];

	char   *Apsz = NULL;
	char   *Xpsz = NULL;
	char   *Cpsz = NULL;
	int    priv_type;

	/* initialize SNMP */
	snmp_sess_init(&session);

	/* Bind to snmp_clientaddr if specified */
	size_t len = strlen(set.snmp_clientaddr);
	if (len > 0 && len <= SMALL_BUFSIZE) {
		#if SNMP_LOCALNAME == 1
		session.localname = strdup(set.snmp_clientaddr);
		#endif
	}

	/* Prevent update of the snmpapp.conf file */
	#ifdef NETSNMP_DS_LIB_DONT_PERSIST_STATE
		netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_DONT_PERSIST_STATE, 1);
	#endif

	/* Prevent update of the snmpapp.conf file */
	#ifdef NETSNMP_DS_LIB_DISABLE_PERSISTENT_LOAD
		netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_DISABLE_PERSISTENT_LOAD, 1);
	#endif

	#ifdef NETSNMP_DS_LIB_DONT_PRINT_UNITS
		netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_DONT_PRINT_UNITS, 1);
	#endif

	netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_QUICK_PRINT, 1);
	netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_QUICKE_PRINT, 1);
	netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_PRINT_BARE_VALUE, 1);
	netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID, NETSNMP_DS_LIB_NUMERIC_TIMETICKS, 1);

	session.securityEngineID    = 0;
	session.securityEngineIDLen = 0;

	session.securityName    = 0;
	session.securityNameLen = 0;

	session.contextEngineID    = 0;
	session.contextEngineIDLen = 0;

	session.contextName    = 0;
	session.contextNameLen = 0;

	session.contextEngineID    = 0;
	session.contextEngineIDLen = 0;

	/* verify snmp version is accurate */
	if (snmp_version == 2) {
		session.version       = SNMP_VERSION_2c;
		session.securityModel = SNMP_SEC_MODEL_SNMPv2c;
	} else if (snmp_version == 1) {
		session.version       = SNMP_VERSION_1;
		session.securityModel = SNMP_SEC_MODEL_SNMPv1;
	} else if (snmp_version == 3) {
		session.version       = SNMP_VERSION_3;
		session.securityModel = USM_SEC_MODEL_NUMBER;
	} else {
		SPINE_LOG(("Device[%i] ERROR: SNMP Version Error for Device '%s'", host_id, hostname));
		snmp_host_init_release(&session, Apsz, Xpsz);
		return 0;
	}

	snprintf(hostnameport, BUFSIZE, "%s:%i", hostname, snmp_port);
	session.peername    = hostnameport;
	session.retries     = set.snmp_retries;
	session.timeout     = (snmp_timeout * 1000); /* net-snmp likes microseconds */

	SPINE_LOG_HIGH(("Device[%i] INFO: SNMP Device '%s' has a timeout of %ld (%d), with %d retries", host_id, hostnameport, session.timeout, snmp_timeout, session.retries));

	if ((snmp_version == 2) || (snmp_version == 1)) {
		session.community     = (unsigned char*) snmp_community;
		session.community_len = strlen(snmp_community);
	} else {
		session.community       = (unsigned char *) Cpsz;
		session.community_len   = 0;

		session.securityName    = snmp_username;
		session.securityNameLen = strlen(session.securityName);

		if (snmp_context && strlen(snmp_context)) {
			session.contextName    = snmp_context;
			session.contextNameLen = strlen(session.contextName);
		}

		if (snmp_engine_id && strlen(snmp_engine_id)) {
			session.contextEngineID    = (unsigned char*) snmp_engine_id;
			session.contextEngineIDLen = strlen(snmp_engine_id);
		}

		/* set the authentication protocol */
		int auth_type = usm_lookup_auth_type(snmp_auth_protocol);
		if (auth_type > 0) {
			const oid *auth_proto;

            auth_proto = sc_get_auth_oid(auth_type, &session.securityAuthProtoLen);
            free(session.securityAuthProto);
            session.securityAuthProto = snmp_duplicate_objid(auth_proto, session.securityAuthProtoLen);
		} else {
			SPINE_LOG(("SNMP: Device[%i] Error auth protocol %s is invalid.", host_id, snmp_auth_protocol));
			snmp_host_init_release(&session, Apsz, Xpsz);
			return 0;
		}

		/* set the privacy protocol to none */
		if (strcmp(snmp_priv_protocol, "[None]") == 0 || (strlen(snmp_priv_passphrase) == 0)) {
			session.securityPrivProto    = snmp_duplicate_objid(usmNoPrivProtocol, OID_LENGTH(usmNoPrivProtocol));
			session.securityPrivProtoLen = OID_LENGTH(usmNoPrivProtocol);
			session.securityPrivKeyLen   = USM_PRIV_KU_LEN;

			/* set the security level to authenticate, but not encrypted */
			if (strlen(snmp_password)) {
				session.securityLevel = SNMP_SEC_LEVEL_AUTHNOPRIV;
			} else {
				session.securityLevel = SNMP_SEC_LEVEL_NOAUTH;
			}
		} else {
			const oid *priv_proto;

			priv_type = usm_lookup_priv_type(snmp_priv_protocol);

			if (priv_type < 0) {
				SPINE_LOG(("SNMP: Device[%i] Error privacy protocol %s is invalid.", host_id, snmp_priv_protocol));
				snmp_host_init_release(&session, Apsz, Xpsz);
				return 0;
			}

			priv_proto = sc_get_priv_oid(priv_type, &session.securityPrivProtoLen);
			free(session.securityPrivProto);
			session.securityPrivProto = snmp_duplicate_objid(priv_proto, session.securityPrivProtoLen);
			session.securityLevel     = SNMP_SEC_LEVEL_AUTHPRIV;

			if (!snmp_set_security_keys(&session, host_id, snmp_password, snmp_priv_passphrase)) {
				snmp_host_init_release(&session, NULL, NULL);
				return NULL;
			}
		}

		SPINE_LOG_MEDIUM(("Device[%i] SNMPv3 Using AuthProto: %s, PrivProto: %s", host_id, snmp_auth_protocol, snmp_priv_protocol));
	}

	/* open SNMP Session */
	thread_mutex_lock(LOCK_SNMP);
	sessp = snmp_sess_open(&session);
	thread_mutex_unlock(LOCK_SNMP);

	if (!sessp) {
		SPINE_LOG_DEVICE(host_id, POLLER_VERBOSITY_MEDIUM, ("ERROR: Device[%i] Problem initializing SNMP session '%s'", host_id, hostname));
	}

	snmp_host_init_release(&session, Apsz, Xpsz);
	return sessp;
}

/*! \fn void snmp_host_cleanup(void *snmp_session)
 *  \brief closes an established snmp session
 *
 *	This function performs cleanup of the snmp sessions once polling is completed
 *  for a host.
 *
 */
void snmp_host_cleanup(void *snmp_session) {
	if (snmp_session != NULL) {
		snmp_sess_close(snmp_session);
	}
}

typedef struct {
	struct snmp_pdu *response;
	int status;
	bool valid_oid;
} snmp_reply_t;

static snmp_reply_t snmp_request_parsed(host_t *host, const oid *name, size_t length, int command) {
	snmp_reply_t reply = {NULL, STAT_DESCRIP_ERROR, TRUE};
	if (host->snmp_session == NULL) return reply;
	struct snmp_pdu *request = snmp_pdu_create(command);
	if (request == NULL) {
		SPINE_LOG(("ERROR: Unable to create SNMP PDU"));
		host->snmp_status = reply.status;
		return reply;
	}
	if (snmp_add_null_var(request, name, length) == NULL) {
		snmp_free_pdu(request);
		reply.status = STAT_ERROR;
		host->snmp_status = reply.status;
		return reply;
	}
	/* Net-SNMP owns and frees request after the synchronous call, including
	 * a failed send; the caller owns only the returned response. */
	reply.status = snmp_sess_synch_response(host->snmp_session, request, &reply.response);
	host->snmp_status = reply.status;
	return reply;
}

static snmp_reply_t snmp_single_request(host_t *host, char *text_oid, int command) {
	snmp_reply_t reply = {NULL, STAT_DESCRIP_ERROR, TRUE};
	if (host->snmp_session == NULL) return reply;
	oid parsed[MAX_OID_LEN];
	size_t length = MAX_OID_LEN;
	if (!snmp_parse_oid(text_oid, parsed, &length)) {
		SPINE_LOG(("Device[%i] ERROR: Problems parsing SNMP OID %s", host->id, text_oid));
		reply.status = STAT_ERROR;
		reply.valid_oid = FALSE;
		host->snmp_status = reply.status;
		return reply;
	}
	return snmp_request_parsed(host, parsed, length, command);
}

static int snmp_format_scalar(char *output, const struct variable_list *variable, bool ascii) {
	char temporary[RESULTS_BUFFER] = {0};
	if (variable->name == NULL || snprint_value(temporary, sizeof(temporary), variable->name, variable->name_length, variable) < 0) {
		SET_UNDEFINED(output);
		return STAT_ERROR;
	}
	if (ascii) {
		if (snprint_asciistring(output, RESULTS_BUFFER, (unsigned char *)temporary, strlen(temporary)) < 0) {
			SET_UNDEFINED(output);
			return STAT_ERROR;
		}
	} else {
		strncopy(output, trim(temporary), RESULTS_BUFFER);
	}
	return STAT_SUCCESS;
}

static int snmp_get_variable(host_t *host, const char *text_oid, const struct variable_list *variable, char *output) {
	switch (variable->type) {
		case SNMP_NOSUCHOBJECT:
			if (strstr(text_oid, ".1.3.6.1.2.1.1.1.0") || strstr(text_oid, ".1.3.6.1.2.1.1.3.0")) {
				SPINE_LOG_HIGH(("DEBUG: OID '%s' for Device[%i], SNMP_NOSUCHOBJECT for sysDesc or sysUptime", text_oid, host->id));
				return snmp_format_scalar(output, variable, FALSE);
			}
			SPINE_LOG_DEBUG(("DEBUG: OID '%s' for Device[%i], SNMP_NOSUCHOBJECT not sysDesc or sysUptime", text_oid, host->id));
			break;
		case SNMP_NOSUCHINSTANCE:
			if (strstr(text_oid, ".1.3.6.1.6.3.10.2.1.3.0")) {
				SPINE_LOG_DEBUG(("NOTE: Legacy SNMP agent found! No per second Uptime oid '%s' for Device[%i]", text_oid, host->id));
			} else {
				SPINE_LOG_HIGH(("WARNING: No such Instance for oid '%s' for Device[%i]", text_oid, host->id));
			}
			break;
		case SNMP_ENDOFMIBVIEW:
			SPINE_LOG_HIGH(("ERROR: End of Mib for oid '%s' for Device[%i]", text_oid, host->id));
			break;
		default: return snmp_format_scalar(output, variable, FALSE);
	}
	SET_UNDEFINED(output);
	return STAT_ERROR;
}

static int snmp_get_response(host_t *host, const char *text_oid, const snmp_reply_t *reply, char *output) {
	if (reply->status == STAT_DESCRIP_ERROR) {
		SET_UNDEFINED(output);
		return STAT_ERROR;
	}
	if (reply->status == STAT_SUCCESS) {
		if (reply->response == NULL) {
			SPINE_LOG(("ERROR: An internal Net-Snmp error condition detected in Cacti snmp_get"));
			SET_UNDEFINED(output);
			return STAT_ERROR;
		}
		if (reply->response->errstat == SNMP_ERR_NOERROR && reply->response->variables != NULL && reply->response->variables->name != NULL) {
			return snmp_get_variable(host, text_oid, reply->response->variables, output);
		}
		/* Preserve the legacy transport-success outcome for an agent error:
		 * availability observes host->snmp_status, while data remains empty. */
		SPINE_LOG_HIGH(("ERROR: Failed to get oid '%s' for Device[%i] with Response[%ld]", text_oid, host->id, reply->response->errstat));
		return STAT_SUCCESS;
	}
	if (reply->response != NULL && reply->response->variables != NULL) {
		SET_UNDEFINED(output);
		SPINE_LOG_HIGH(("ERROR: Agent error getting oid '%s' for Device[%i] with Status[%d]", text_oid, host->id, reply->status));
		return STAT_ERROR;
	}
	SPINE_LOG_HIGH(("ERROR: %s getting oid '%s' for Device[%i] with Status[%d]", reply->status == STAT_TIMEOUT ? "Timeout" : "Unknown error", text_oid, host->id, reply->status));
	return reply->status;
}

/*! \fn char *snmp_get_base(host_t *current_host, char *snmp_oid, bool should_fail)
 *  \brief performs a single snmp_get for a specific snmp OID
 *
 *	This function will poll a specific snmp OID for a host.  The host snmp
 *  session must already be established.
 *
 *  \return returns the character representaton of the snmp OID, or "U" if
 *  unsuccessful.
 *
 */
char *snmp_get_base(host_t *host, char *text_oid, bool should_fail) {
	char *output = calloc(RESULTS_BUFFER, 1);
	if (output == NULL) die("ERROR: Fatal malloc error: snmp.c snmp_get!");
	if (host->ignore_host) {
		SPINE_LOG_HIGH(("WARNING: Skipped oid '%s' for Device[%i] as host ignore flag is active", text_oid, host->id));
		SET_UNDEFINED(output);
		return output;
	}
	snmp_reply_t reply = snmp_single_request(host, text_oid, SNMP_MSG_GET);
	if (!reply.valid_oid) {
		SET_UNDEFINED(output);
		return output;
	}
	int status = reply.status;
	if (host->snmp_session != NULL) status = snmp_get_response(host, text_oid, &reply, output);
	if (reply.response != NULL) snmp_free_pdu(reply.response);
	if (status != STAT_SUCCESS && should_fail) {
		host->ignore_host = TRUE;
		SET_UNDEFINED(output);
	}
	return output;
}

char *snmp_get(host_t *host, char *text_oid) {
	return snmp_get_base(host, text_oid, TRUE);
}

char *snmp_getnext(host_t *host, char *text_oid) {
	char *output = calloc(RESULTS_BUFFER, 1);
	if (output == NULL) die("ERROR: Fatal malloc error: snmp.c snmp_getnext!");
	snmp_reply_t reply = snmp_single_request(host, text_oid, SNMP_MSG_GETNEXT);
	if (!reply.valid_oid) {
		SET_UNDEFINED(output);
		return output;
	}
	int status = reply.status;
	if (status == STAT_SUCCESS) {
		if (reply.response == NULL) {
			SPINE_LOG(("ERROR: An internal Net-Snmp error condition detected in Cacti snmp_getnext"));
			status = STAT_ERROR;
		} else if (reply.response->errstat == SNMP_ERR_NOERROR) {
			status = reply.response->variables == NULL ? STAT_ERROR : snmp_format_scalar(output, reply.response->variables, TRUE);
		}
	}
	if (reply.response != NULL) snmp_free_pdu(reply.response);
	if (status != STAT_SUCCESS) {
		host->ignore_host = TRUE;
		SET_UNDEFINED(output);
	}
	return output;
}

typedef struct {
	oid root[MAX_OID_LEN];
	size_t root_length;
	oid current[MAX_OID_LEN];
	size_t current_length;
	int count;
	bool failed;
} snmp_walk_t;

static bool snmp_count_advance(snmp_walk_t *walk, const struct variable_list *variable) {
	if (variable->name == NULL || variable->name_length > MAX_OID_LEN) {
		walk->failed = TRUE;
		return FALSE;
	}
	if (variable->name_length < walk->root_length || memcmp(walk->root, variable->name, walk->root_length * sizeof(oid)) != 0) return FALSE;
	if (walk->count == INT_MAX) {
		SPINE_LOG(("ERROR: SNMP table count exceeds supported integer range"));
		walk->failed = TRUE;
		return FALSE;
	}
	walk->count++;
	/* Preserve the legacy count for an exception returned inside the root. */
	if (variable->type == SNMP_ENDOFMIBVIEW || variable->type == SNMP_NOSUCHOBJECT || variable->type == SNMP_NOSUCHINSTANCE) return FALSE;
	if (snmp_oid_compare(walk->current, walk->current_length, variable->name, variable->name_length) >= 0) {
		SPINE_LOG(("ERROR: OID not increasing"));
		walk->failed = TRUE;
		return FALSE;
	}
	memcpy(walk->current, variable->name, variable->name_length * sizeof(oid));
	walk->current_length = variable->name_length;
	return TRUE;
}

static bool snmp_count_response(snmp_walk_t *walk, const snmp_reply_t *reply) {
	if (reply->status != STAT_SUCCESS || reply->response == NULL || reply->response->errstat != SNMP_ERR_NOERROR || reply->response->variables == NULL) {
		SPINE_LOG(("ERROR: %s detected in Cacti snmp_count", reply->status == STAT_TIMEOUT ? "Timeout" : "Invalid SNMP response"));
		walk->failed = TRUE;
		return FALSE;
	}
	for (const struct variable_list *variable = reply->response->variables; variable != NULL; variable = variable->next_variable) {
		if (!snmp_count_advance(walk, variable)) return FALSE;
	}
	return TRUE;
}

/*! \fn char *snmp_count(host_t *current_host, char *snmp_oid)
 *  \brief counts entries of snmp table specified by a specific snmp OID
 *
 *	This function will poll a specific snmp OID for a host.  The host snmp
 *  session must already be established.
 *
 *  \return returns count of table entries
 *
 */
int snmp_count(host_t *host, char *text_oid) {
	SPINE_LOG_DEVICE(host->id, POLLER_VERBOSITY_DEBUG, ("DEBUG: walk starts at OID %s", text_oid));
	if (host->snmp_session == NULL) {
		host->ignore_host = TRUE;
		return 0;
	}
	snmp_walk_t walk = {0};
	walk.root_length = MAX_OID_LEN;
	if (!snmp_parse_oid(text_oid, walk.root, &walk.root_length)) {
		SPINE_LOG(("Device[%i] ERROR: SNMP Count Problems parsing SNMP OID %s", host->id, text_oid));
		return 0;
	}
	memcpy(walk.current, walk.root, walk.root_length * sizeof(oid));
	walk.current_length = walk.root_length;
	bool more = TRUE;
	while (more) {
		snmp_reply_t reply = snmp_request_parsed(host, walk.current, walk.current_length, SNMP_MSG_GETNEXT);
		more = snmp_count_response(&walk, &reply);
		if (reply.response != NULL) snmp_free_pdu(reply.response);
	}
	if (walk.failed) {
		host->ignore_host = TRUE;
		if (host->snmp_status == STAT_SUCCESS) host->snmp_status = STAT_ERROR;
	}
	return walk.count;
}

/*! \fn void snmp_snprint_value(char *obuf, size_t buf_len, const oid *objid, size_t objidlen, const struct variable_list *variable)
 *
 *  \brief replacement for the buggy net-snmp.org snprint_value function
 *
 *	This function format an output buffer with the correct string representation
 *  of an snmp OID result fetched with snmp_get_multi.  The buffer pointed to by
 *  the function is modified.
 *
 */
void snmp_snprint_value(char *obuf, size_t buf_len, const oid *objid, size_t objidlen, const struct variable_list *variable) {
	u_char *buf    = NULL;
	size_t out_len = 0;
	(void)objid;
	(void)objidlen;

	if (buf_len > 0) {
		if ((buf = (u_char *) calloc(buf_len, 1)) != 0) {
			sprint_realloc_by_type(&buf, &buf_len, &out_len, 0, variable, NULL, NULL, NULL);
			snprintf(obuf, buf_len, "%s", buf);
		} else {
			SET_UNDEFINED(obuf);
		}

		free(buf);
	} else {
		SET_UNDEFINED(obuf);
	}
}

static void snmp_multi_undefined(snmp_oids_t *oids, int count) {
	for (int index = 0; index < count; index++) SET_UNDEFINED(oids[index].result);
}

static struct snmp_pdu *snmp_multi_request(host_t *host, const target_t *items, snmp_oids_t *oids, int count) {
	struct snmp_pdu *request = snmp_pdu_create(SNMP_MSG_GET);
	if (request == NULL) return NULL;
	for (int index = 0; index < count; index++) {
		if (IS_UNDEFINED(oids[index].result)) continue;
		oid name[MAX_OID_LEN];
		size_t length = MAX_OID_LEN;
		if (!snmp_parse_oid(oids[index].oid, name, &length)) {
			SPINE_LOG(("Device[%i] DS[%i] ERROR: Problems parsing Multi SNMP OID! (oid: %s), Set MAX_OIDS to 1 for this host to isolate bad OID", host->id, items[oids[index].array_position].local_data_id, oids[index].oid));
			SET_UNDEFINED(oids[index].result);
		} else if (snmp_add_null_var(request, name, length) == NULL) {
			SET_UNDEFINED(oids[index].result);
		}
	}
	if (request->variables == NULL) {
		snmp_free_pdu(request);
		return NULL;
	}
	return request;
}

static bool snmp_multi_error_index(snmp_oids_t *oids, int count, long error_index) {
	long current = 0;
	for (int index = 0; index < count; index++) {
		if (IS_UNDEFINED(oids[index].result)) continue;
		if (++current == error_index) {
			SET_UNDEFINED(oids[index].result);
			return TRUE;
		}
	}
	return FALSE;
}

static void snmp_multi_values(snmp_oids_t *oids, int count, const struct variable_list *variable) {
	for (int index = 0; index < count; index++) {
		if (IS_UNDEFINED(oids[index].result)) continue;
		if (variable == NULL) {
			SET_UNDEFINED(oids[index].result);
			continue;
		}
		snmp_format_scalar(oids[index].result, variable, FALSE);
		variable = variable->next_variable;
	}
}

/*! \fn char *snmp_get_multi(host_t *current_host, const target_t *poller_items, snmp_oids_t *snmp_oids, int num_oids)
 *  \brief performs multiple OID snmp_get's in a single network call
 *
 *	This function will a group of snmp OID's for a host.  The host snmp
 *  session must already be established.  The function will modify elements of
 *  the snmp_oids array with the results from the snmp api call.
 *
 */
void snmp_get_multi(host_t *host, const target_t *items, snmp_oids_t *oids, int count) {
	if (count <= 0) return;
	if (host == NULL || items == NULL || oids == NULL) die("ERROR: Invalid multi-SNMP request storage");
	if (host->snmp_session == NULL) {
		snmp_multi_undefined(oids, count);
		host->snmp_status = STAT_DESCRIP_ERROR;
		return;
	}
	struct snmp_pdu *request = snmp_multi_request(host, items, oids, count);
	if (request == NULL) {
		snmp_multi_undefined(oids, count);
		host->snmp_status = STAT_ERROR;
		return;
	}
	int status = STAT_DESCRIP_ERROR;
	/* Every v1 retry removes one valid OID, so at most count requests are
	 * possible. The synchronous call consumes each submitted request. */
	for (int attempt = 0; request != NULL && attempt < count; attempt++) {
		struct snmp_pdu *response = NULL;
		status = snmp_sess_synch_response(host->snmp_session, request, &response);
		request = NULL;
		host->snmp_status = status;
		if (status == STAT_SUCCESS) {
			if (response == NULL) {
				SPINE_LOG(("ERROR: An internal Net-Snmp error condition detected in Cacti snmp_get_multi"));
				status = STAT_ERROR;
				snmp_multi_undefined(oids, count);
			} else if (response->errstat == SNMP_ERR_NOERROR) {
				snmp_multi_values(oids, count, response->variables);
			} else if (snmp_multi_error_index(oids, count, response->errindex)) {
				request = snmp_fix_pdu(response, SNMP_MSG_GET);
			} else {
				status = STAT_ERROR;
				snmp_multi_undefined(oids, count);
			}
		}
		if (response != NULL) snmp_free_pdu(response);
	}
	if (request != NULL) {
		snmp_free_pdu(request);
		status = STAT_ERROR;
		snmp_multi_undefined(oids, count);
	}
	if (status == STAT_TIMEOUT) {
		host->ignore_host = TRUE;
		snmp_multi_undefined(oids, count);
	}
}
