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
#include "snmp.h"
#include "test_platform_helpers.h"

extern int *debug_devices;
extern int __real_snmp_sess_synch_response(void *session, struct snmp_pdu *pdu,
	struct snmp_pdu **response);
extern void __real_snmp_sess_error(void *session, int *system_error, int *library_error,
	char **description);
extern int __real_snmp_sess_close(void *session);
extern void __real_free(void *pointer);

static void *owned_session;
static char *owned_description;
static int injection_active;
static int request_count;
static int error_count;
static int description_free_count;
static int invalid_close_count;
static int valid_close_count;
static int events;
static int request_event;
static int error_event;
static int release_event;
static int expected_command;
static int expected_library_error;

#if defined(SNMPERR_NOT_IN_TIME_WINDOW)
#define TEST_TIME_WINDOW_ERROR SNMPERR_NOT_IN_TIME_WINDOW
#elif defined(SNMPERR_USM_NOTINTIMEWINDOW)
#define TEST_TIME_WINDOW_ERROR SNMPERR_USM_NOTINTIMEWINDOW
#else
#error "SNMP diagnostic recovery regression requires a supported USM time-window error"
#endif

int __wrap_snmp_sess_synch_response(void *session, struct snmp_pdu *pdu,
	struct snmp_pdu **response) {
	if (!injection_active) return __real_snmp_sess_synch_response(session, pdu, response);
	ASSERT_TRUE(session == owned_session);
	ASSERT_TRUE(pdu != NULL && pdu->command == expected_command);
	ASSERT_TRUE(response != NULL && *response == NULL);
	request_count++;
	request_event = ++events;
	struct snmp_session *native = snmp_sess_session(session);
	ASSERT_TRUE(native != NULL);
	if (native != NULL) {
		native->s_errno = EAFNOSUPPORT;
		native->s_snmp_errno = expected_library_error;
	}
	/* A synchronous request consumes the caller's PDU. Free the real PDU;
	 * return no response instead of supplying a fabricated SNMP reply. */
	snmp_free_pdu(pdu);
	if (response != NULL) *response = NULL;
	return STAT_ERROR;
}

void __wrap_snmp_sess_error(void *session, int *system_error, int *library_error,
	char **description) {
	ASSERT_TRUE(session == owned_session && injection_active);
	ASSERT_TRUE(request_count == 1 && error_count == 0);
	error_count++;
	error_event = ++events;
	__real_snmp_sess_error(session, system_error, library_error, description);
	/* Installed output_api.h puts C/system errno first, SNMP library errno
	 * second. Observe the actual API rather than inventing a diagnostic. */
	ASSERT_TRUE(*system_error == EAFNOSUPPORT && *library_error == expected_library_error);
	ASSERT_TRUE(description != NULL && *description != NULL && **description != '\0');
	if (description != NULL) owned_description = *description;
}

void __wrap_free(void *pointer) {
	if (injection_active && pointer != NULL && pointer == owned_description) {
		ASSERT_TRUE(error_count == 1 && description_free_count == 0);
		description_free_count++;
		release_event = ++events;
		owned_description = NULL;
	}
	__real_free(pointer);
}

int __wrap_snmp_sess_close(void *session) {
	if (session != owned_session) {
		/* The historical bug routes an error string here. Reject without
		 * dereferencing it, preserving a clean native negative-control fail. */
		invalid_close_count++;
		ASSERT_TRUE(session == owned_session);
		return 0;
	}
	valid_close_count++;
	return __real_snmp_sess_close(session);
}

static void test_request_error(spine_spine_host_t *host, int get_next, int recoverable) {
	owned_description = NULL;
	request_count = 0;
	error_count = 0;
	description_free_count = 0;
	invalid_close_count = 0;
	events = 0;
	request_event = 0;
	error_event = 0;
	release_event = 0;
	expected_command = get_next ? SNMP_MSG_GETNEXT : SNMP_MSG_GET;
	expected_library_error = recoverable ? TEST_TIME_WINDOW_ERROR : SNMPERR_BAD_SENDTO;
	host->ignore_host = FALSE;
	host->snmp_status = STAT_SUCCESS;
	injection_active = 1;
	char *result = get_next ? snmp_getnext(host, ".1.3.6.1.2.1.1.3.0") :
		snmp_get_base(host, ".1.3.6.1.2.1.1.3.0", true);
	ASSERT_TRUE(result != NULL && IS_UNDEFINED(result));
	/* System EAFNOSUPPORT must not overwrite the library classification.
	 * A USM time-window error keeps the host eligible for subsequent polls. */
	ASSERT_TRUE(host->snmp_status == (recoverable ? STAT_SUCCESS : STAT_ERROR));
	ASSERT_TRUE(host->ignore_host == (recoverable ? FALSE : TRUE));
	ASSERT_TRUE(request_count == 1 && error_count == 1 && description_free_count == 1);
	ASSERT_TRUE(request_event == 1 && error_event == 2 && release_event == 3);
	ASSERT_TRUE(invalid_close_count == 0 && valid_close_count == 0);
	ASSERT_TRUE(host->snmp_session == owned_session);
	ASSERT_TRUE(snmp_sess_session(owned_session) != NULL);
	injection_active = 0;
	free(result);
	if (owned_description != NULL) {
		/* Only the negative control leaves this owned diagnostic unfreed. */
		__real_free(owned_description);
		owned_description = NULL;
	}
}

int main(void) {
	const int platform_status = spine_platform_init();
	ASSERT_TRUE(platform_status == 0);
	if (platform_status != 0) return finish_tests("SNMP error cleanup platform initialization");
	int owned_debug_devices[100] = {0};
	debug_devices = owned_debug_devices;
	set.snmp_retries = 0;
	snmp_spine_init();
	char hostname[] = "127.0.0.1";
	char community[] = "public";
	char empty[] = "";
	char auth_protocol[] = "SHA";
	char priv_protocol[] = "[None]";
	owned_session = snmp_host_init(&(spine_snmp_profile_t){
			.host_id = 1,
			.hostname = hostname,
			.snmp_version = 2,
			.snmp_community = community,
			.snmp_username = empty,
			.snmp_password = empty,
			.snmp_auth_protocol = auth_protocol,
			.snmp_priv_passphrase = empty,
			.snmp_priv_protocol = priv_protocol,
			.snmp_context = empty,
			.snmp_engine_id = empty,
			.snmp_port = 1161,
			.snmp_timeout = 100,
		});
	ASSERT_TRUE(owned_session != NULL);
	if (owned_session != NULL) {
		spine_spine_host_t host = {0};
		host.id = 1;
		host.snmp_session = owned_session;
		test_request_error(&host, 0, 0);
		test_request_error(&host, 1, 0);
		test_request_error(&host, 0, 1);
		test_request_error(&host, 1, 1);
		snmp_host_cleanup(owned_session);
		ASSERT_TRUE(valid_close_count == 1);
		owned_session = NULL;
	}
	snmp_spine_close();
	spine_platform_cleanup();
	debug_devices = NULL;
	return finish_tests("SNMP diagnostic string ownership");
}
