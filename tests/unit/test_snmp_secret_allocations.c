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

/* GNU/Linux allocator fault test. Successful calls use real strdup/free and
 * real Net-SNMP session construction; only one selected owned copy may fail. */
extern int *debug_devices;
extern char *__real_strdup(const char *value);
extern void __real_free(void *pointer);

static const char auth_secret[] = "allocation-auth-password";
static const char priv_secret[] = "allocation-privacy-password";
static void *owned_copies[2];
static size_t owned_lengths[2];
static int capture_active;
static int copy_attempts;
static int copies_released;
static int failure_index;
static int faults_observed;

char *__wrap_strdup(const char *value) {
	const int selected = capture_active && value != NULL &&
		(strcmp(value, auth_secret) == 0 || strcmp(value, priv_secret) == 0);
	if (!selected) return __real_strdup(value);
	const int index = copy_attempts++;
	ASSERT_TRUE(index < 2);
	if (index >= 2) return NULL;
	if (index == failure_index) {
		faults_observed++;
		errno = ENOMEM;
		return NULL;
	}
	char *copy = __real_strdup(value);
	ASSERT_TRUE(copy != NULL);
	owned_copies[index] = copy;
	owned_lengths[index] = strlen(value);
	return copy;
}

void __wrap_free(void *pointer) {
	if (capture_active && pointer != NULL) {
		for (int index = 0; index < 2; index++) {
			if (pointer != owned_copies[index]) continue;
			/* Observe production's actual buffer before the real allocator
			 * frees it; never inspect already-freed memory. */
			const unsigned char *bytes = pointer;
			for (size_t offset = 0; offset < owned_lengths[index]; offset++) {
				ASSERT_TRUE(bytes[offset] == 0);
			}
			owned_copies[index] = NULL;
			copies_released++;
			break;
		}
	}
	__real_free(pointer);
}

static void test_owned_copy_allocation(int failing_index) {
	char hostname[] = "127.0.0.1";
	char community[] = "public";
	char username[] = "regression-user";
	char authentication[sizeof(auth_secret)];
	char privacy[sizeof(priv_secret)];
	char auth_protocol[] = "SHA";
	char priv_protocol[] = "AES";
	char context[] = "";
	char engine_id[] = "";
	memcpy(authentication, auth_secret, sizeof(authentication));
	memcpy(privacy, priv_secret, sizeof(privacy));
	memset(owned_copies, 0, sizeof(owned_copies));
	memset(owned_lengths, 0, sizeof(owned_lengths));
	copy_attempts = 0;
	copies_released = 0;
	faults_observed = 0;
	failure_index = failing_index;
	capture_active = 1;
	void *handle = spine_snmp_profile_open(&(spine_snmp_profile_t){
			.host_id = 1,
			.hostname = hostname,
			.snmp_version = 3,
			.snmp_community = community,
			.snmp_username = username,
			.snmp_password = authentication,
			.snmp_auth_protocol = auth_protocol,
			.snmp_priv_passphrase = privacy,
			.snmp_priv_protocol = priv_protocol,
			.snmp_context = context,
			.snmp_engine_id = engine_id,
			.snmp_port = 1161,
			.snmp_timeout = 100,
		});
	ASSERT_TRUE(copy_attempts == 2);
	ASSERT_TRUE(faults_observed == (failing_index < 0 ? 0 : 1));
	ASSERT_TRUE(copies_released == (failing_index < 0 ? 2 : 1));
	ASSERT_TRUE(owned_copies[0] == NULL && owned_copies[1] == NULL);
	ASSERT_TRUE(memcmp(authentication, auth_secret, sizeof(authentication)) == 0);
	ASSERT_TRUE(memcmp(privacy, priv_secret, sizeof(privacy)) == 0);
	ASSERT_TRUE(failing_index < 0 ? handle != NULL : handle == NULL);
	if (handle != NULL) {
		const struct snmp_session *session = snmp_sess_session(handle);
		ASSERT_TRUE(session != NULL && session->securityLevel == SNMP_SEC_LEVEL_AUTHPRIV);
		snmp_host_cleanup(handle);
	}
	capture_active = 0;
}

int main(void) {
	const int platform_status = spine_platform_init();
	ASSERT_TRUE(platform_status == 0);
	if (platform_status != 0) return finish_tests("SNMP platform initialization");
	int owned_debug_devices[100] = {0};
	debug_devices = owned_debug_devices;
	set.snmp_retries = 0;
	snmp_spine_init();
	test_owned_copy_allocation(-1);
	test_owned_copy_allocation(0);
	test_owned_copy_allocation(1);
	snmp_spine_close();
	spine_platform_cleanup();
	debug_devices = NULL;
	return finish_tests("SNMP private-copy allocation and scrubbing");
}
