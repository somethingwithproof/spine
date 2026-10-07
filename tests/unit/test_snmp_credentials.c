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

/* Public regression credentials only. No live agent or database is required:
 * opening/inspecting a real Net-SNMP session does not request an OID. */
typedef struct {
	char hostname[32];
	char community[32];
	char username[32];
	char authentication[64];
	char auth_protocol[16];
	char privacy[64];
	char priv_protocol[16];
	char context[32];
	char engine_id[32];
} credential_profile_t;

static credential_profile_t make_profile(const char *authentication, const char *privacy) {
	credential_profile_t profile = {0};
	snprintf(profile.hostname, sizeof(profile.hostname), "%s", "127.0.0.1");
	snprintf(profile.community, sizeof(profile.community), "%s", "public");
	snprintf(profile.username, sizeof(profile.username), "%s", "regression-user");
	snprintf(profile.authentication, sizeof(profile.authentication), "%s", authentication);
	snprintf(profile.auth_protocol, sizeof(profile.auth_protocol), "%s", "SHA");
	snprintf(profile.privacy, sizeof(profile.privacy), "%s", privacy);
	snprintf(profile.priv_protocol, sizeof(profile.priv_protocol), "%s", "AES");
	snprintf(profile.context, sizeof(profile.context), "%s", "regression-context");
	return profile;
}

static void *open_profile(credential_profile_t *profile, int legacy) {
	if (legacy) {
		return snmp_host_init(1, profile->hostname, 3, profile->community,
			profile->username, profile->authentication, profile->auth_protocol,
			profile->privacy, profile->priv_protocol, profile->context,
			profile->engine_id, 1161, 100);
	}
	return spine_snmp_profile_open(&(spine_snmp_profile_t){
			.host_id = 1,
			.hostname = profile->hostname,
			.snmp_version = 3,
			.snmp_community = profile->community,
			.snmp_username = profile->username,
			.snmp_password = profile->authentication,
			.snmp_auth_protocol = profile->auth_protocol,
			.snmp_priv_passphrase = profile->privacy,
			.snmp_priv_protocol = profile->priv_protocol,
			.snmp_context = profile->context,
			.snmp_engine_id = profile->engine_id,
			.snmp_port = 1161,
			.snmp_timeout = 100,
		});
}

static void test_repeated_authpriv_creation(void) {
	credential_profile_t profile = make_profile("regression-auth-password", "regression-privacy-password");
	const credential_profile_t before = profile;
	u_char auth_key[USM_AUTH_KU_LEN] = {0};
	u_char priv_key[USM_PRIV_KU_LEN] = {0};
	size_t auth_key_length = 0;
	size_t priv_key_length = 0;
	for (int iteration = 0; iteration < 2; iteration++) {
		void *handle = open_profile(&profile, iteration == 0);
		ASSERT_TRUE(memcmp(&profile, &before, sizeof(profile)) == 0);
		ASSERT_TRUE(handle != NULL);
		if (handle == NULL) {
			fprintf(stderr, "Net-SNMP session admission failed: %s (errno=%d)\n",
				snmp_api_errstring(snmp_errno), errno);
			continue;
		}
		const struct snmp_session *session = snmp_sess_session(handle);
		ASSERT_TRUE(session != NULL);
		if (session != NULL) {
			ASSERT_TRUE(session->version == SNMP_VERSION_3);
			ASSERT_TRUE(session->securityLevel == SNMP_SEC_LEVEL_AUTHPRIV);
			ASSERT_TRUE(session->securityAuthKeyLen == 20); /* SHA-1 Ku digest length. */
			ASSERT_TRUE(session->securityPrivKeyLen > 0 && session->securityPrivKeyLen <= sizeof(priv_key));
			if (session->securityAuthKeyLen <= sizeof(auth_key) && session->securityPrivKeyLen <= sizeof(priv_key)) {
				if (iteration == 0) {
					auth_key_length = session->securityAuthKeyLen;
					priv_key_length = session->securityPrivKeyLen;
					memcpy(auth_key, session->securityAuthKey, auth_key_length);
					memcpy(priv_key, session->securityPrivKey, priv_key_length);
				} else {
					ASSERT_TRUE(session->securityAuthKeyLen == auth_key_length &&
						memcmp(session->securityAuthKey, auth_key, auth_key_length) == 0);
					ASSERT_TRUE(session->securityPrivKeyLen == priv_key_length &&
						memcmp(session->securityPrivKey, priv_key, priv_key_length) == 0);
				}
			}
		}
		snmp_host_cleanup(handle);
		ASSERT_TRUE(memcmp(&profile, &before, sizeof(profile)) == 0);
	}
}

static void test_failed_key_derivation_preserves_profile(void) {
	const char *authentication[] = {"short", "regression-auth-password"};
	const char *privacy[] = {"regression-privacy-password", "short"};
	for (size_t index = 0; index < sizeof(authentication) / sizeof(authentication[0]); index++) {
		credential_profile_t profile = make_profile(authentication[index], privacy[index]);
		const credential_profile_t before = profile;
		void *handle = open_profile(&profile, 0);
		ASSERT_TRUE(handle == NULL);
		ASSERT_TRUE(memcmp(&profile, &before, sizeof(profile)) == 0);
		if (handle != NULL) snmp_host_cleanup(handle);
	}
}

int main(void) {
	ASSERT_TRUE(spine_snmp_profile_open(NULL) == NULL);
	const int platform_status = spine_platform_init();
	ASSERT_TRUE(platform_status == 0);
	if (platform_status != 0) return finish_tests("SNMP platform initialization");
	int owned_debug_devices[100] = {0};
	debug_devices = owned_debug_devices;
	set.snmp_retries = 0;
	snmp_spine_init();
	test_repeated_authpriv_creation();
	test_failed_key_derivation_preserves_profile();
	snmp_spine_close();
	spine_platform_cleanup();
	debug_devices = NULL;
	return finish_tests("SNMP caller credential preservation");
}
