/* Characterization tests: these expectations also pass at baseline
 * 8b04bdabeb33bca080533badeee5472f9a953bac. Preserve quirks explicitly. */
#include "common.h"
#include "platform/platform_socket.h"
#include "spine.h"
#include "test_platform_helpers.h"

static int connect_result, connect_calls, send_calls, close_calls;
static int mock_snmp_status, snmp_calls, tcp_retries = 1;
static int time_ticks;
static char last_oid[80];
static struct sockaddr_in destination;
static struct addrinfo resolution;
static int behavior_resolve(const char *node, const char *service,
                            const struct addrinfo *hints,
                            struct addrinfo **out) {
  (void)node;
  (void)service;
  (void)hints;
  destination = (struct sockaddr_in){.sin_family = AF_INET};
  resolution = (struct addrinfo){.ai_family = AF_INET,
                                 .ai_addrlen = sizeof(destination),
                                 .ai_addr = (struct sockaddr *)&destination};
  *out = &resolution;
  return 0;
}
static void behavior_free(struct addrinfo *info) { (void)info; }
static spine_socket_t behavior_open(int domain, int type, int protocol) {
  (void)domain;
  (void)type;
  (void)protocol;
  return 7;
}
static int behavior_close(spine_socket_t fd) {
  (void)fd;
  close_calls++;
  return 0;
}
static int behavior_connect(spine_socket_t fd, const struct sockaddr *addr,
                            socklen_t len) {
  (void)fd;
  (void)addr;
  (void)len;
  connect_calls++;
  errno = ECONNREFUSED;
  return connect_result;
}
static int behavior_timeout(spine_socket_t fd, const struct timeval *timeout) {
  (void)fd;
  (void)timeout;
  return 0;
}
static int behavior_retries(void) { return tcp_retries; }
static int behavior_wait(spine_socket_t fd, struct timeval *timeout) {
  (void)fd;
  (void)timeout;
  return 0;
}
static int behavior_send(spine_socket_t fd, const void *buf, size_t length,
                         int flags) {
  (void)fd;
  (void)buf;
  (void)flags;
  send_calls++;
  return (int)length;
}
static void behavior_sleep(unsigned int usec) { (void)usec; }
static double behavior_time(void) { return time_ticks++ / 1000.0; }

static int behavior_debug(int id) {
  (void)id;
  return FALSE;
}
static int behavior_log(const char *format, ...) {
  (void)format;
  return 0;
}
#define is_debug_device behavior_debug
#define spine_log behavior_log
#define getaddrinfo behavior_resolve
#define freeaddrinfo behavior_free
#define spine_socket_open behavior_open
#define spine_socket_close behavior_close
#define spine_socket_connect behavior_connect
#define spine_socket_set_timeout behavior_timeout
#define spine_socket_ping_tcp_supports_retries behavior_retries
#define spine_socket_wait_readable behavior_wait
#define spine_socket_send behavior_send
#define spine_platform_sleep_us behavior_sleep
#define get_time_as_double behavior_time
#include "../../src/ping.c"

char *snmp_get(host_t *host, const char *oid) {
  snmp_calls++;
  snprintf(last_oid, sizeof(last_oid), "%s", oid);
  host->snmp_status = mock_snmp_status;
  return strdup("value");
}
char *snmp_getnext(host_t *host, const char *oid) {
  return snmp_get(host, oid);
}

static host_t make_host(void) {
  host_t host = {.ping_method = PING_TCP,
                 .ping_timeout = 1000,
                 .ping_retries = 2,
                 .snmp_version = 2,
                 .snmp_session = &resolution};
  snprintf(host.hostname, sizeof(host.hostname), "127.0.0.1");
  snprintf(host.snmp_community, sizeof(host.snmp_community), "test");
  return host;
}

static void test_availability_matrix(void) {
  for (int method = AVAIL_NONE; method <= AVAIL_STREAM; method++) {
    for (int tcp_up = 0; tcp_up <= 1; tcp_up++) {
      for (int snmp_up = 0; snmp_up <= 1; snmp_up++) {
        host_t host = make_host();
        ping_t ping = {0};
        host.availability_method = method;
        connect_result = tcp_up ? 0 : -1;
        mock_snmp_status = snmp_up ? SNMPERR_SUCCESS : STAT_TIMEOUT;
        snmp_calls = 0;
        int expected_up = 0;
        switch (method) {
        case AVAIL_NONE:
          expected_up = 1;
          break;
        case AVAIL_SNMP_AND_PING:
          expected_up = tcp_up && snmp_up;
          break;
        case AVAIL_PING:
          expected_up = tcp_up;
          break;
        /* Baseline skips SNMP in OR mode and defaults it to UP. */
        case AVAIL_SNMP_OR_PING:
          expected_up = 1;
          break;
        case AVAIL_SNMP:
        case AVAIL_SNMP_GET_SYSDESC:
        case AVAIL_SNMP_GET_NEXT:
          expected_up = snmp_up;
          break;
        }
        ASSERT_INT_EQ(ping_host(&host, &ping),
                      expected_up ? HOST_UP : HOST_DOWN);
        if (method == AVAIL_SNMP_OR_PING ||
            (method == AVAIL_SNMP_AND_PING && !tcp_up)) {
          ASSERT_INT_EQ(snmp_calls, 0);
        }
      }
    }
  }
}

static void test_retry_and_format_contract(void) {
  host_t host = make_host();
  ping_t ping = {0};
  connect_result = -1;
  connect_calls = close_calls = 0;
  ASSERT_INT_EQ(ping_tcp(&host, &ping), HOST_DOWN);
  /* Existing POSIX TCP retries are retries + 2, unlike UDP/ICMP. */
  ASSERT_INT_EQ(connect_calls, 4);
  ASSERT_INT_EQ(close_calls, 1);
  ASSERT_TRUE(strcmp(ping.ping_response, "TCP: Cannot connect to host") == 0);
  ASSERT_TRUE(strcmp(ping.ping_status, "down") == 0);
  tcp_retries = 0;
  connect_calls = 0;
  ASSERT_INT_EQ(ping_tcp(&host, &ping), HOST_DOWN);
  ASSERT_INT_EQ(connect_calls, 1);
  tcp_retries = 1;
  host.ping_method = PING_TCP_CLOSED;
  time_ticks = 0;
  ASSERT_INT_EQ(ping_tcp(&host, &ping), HOST_UP);
  ASSERT_TRUE(strcmp(ping.ping_response, "TCP: Device is Alive") == 0);
  ASSERT_TRUE(strcmp(ping.ping_status, "1.00000") == 0);
  connect_result = 0;
  send_calls = 0;
  ASSERT_INT_EQ(ping_udp(&host, &ping), HOST_DOWN);
  ASSERT_INT_EQ(send_calls, 3);
  ASSERT_TRUE(strcmp(ping.ping_response, "UDP: Ping timed out") == 0);
  host.availability_method = AVAIL_PING;
  snprintf(host.hostname, sizeof(host.hostname), "localhost");
  ASSERT_INT_EQ(ping_host(&host, &ping), HOST_UP);
  ASSERT_TRUE(strcmp(ping.ping_status, "0.000") == 0);
  ASSERT_TRUE(
      strcmp(ping.ping_response, "PING: Device does not require ping.") == 0);
}

static void test_snmp_contract(void) {
  host_t host = make_host();
  ping_t ping = {0};
  const int methods[] = {AVAIL_SNMP, AVAIL_SNMP_GET_SYSDESC,
                         AVAIL_SNMP_GET_NEXT};
  const char *oids[] = {".1.3.6.1.2.1.1.3.0", ".1.3.6.1.2.1.1.1.0", ".1.3"};
  for (size_t i = 0; i < 3; i++) {
    host.availability_method = methods[i];
    mock_snmp_status = SNMPERR_SUCCESS;
    time_ticks = 0;
    ASSERT_INT_EQ(ping_snmp(&host, &ping), HOST_UP);
    ASSERT_TRUE(strcmp(last_oid, oids[i]) == 0);
    ASSERT_TRUE(strcmp(ping.snmp_status, "1.00000") == 0);
  }
  mock_snmp_status = SNMPERR_UNKNOWN_OBJID;
  ASSERT_INT_EQ(ping_snmp(&host, &ping), HOST_UP);
  mock_snmp_status = STAT_TIMEOUT;
  snprintf(ping.snmp_status, sizeof(ping.snmp_status), "previous");
  ASSERT_INT_EQ(ping_snmp(&host, &ping), HOST_DOWN);
  ASSERT_TRUE(strcmp(ping.snmp_status, "previous") == 0);
  ASSERT_TRUE(strcmp(ping.snmp_response, "Device did not respond to SNMP") ==
              0);
  host.snmp_community[0] = '\0';
  ASSERT_INT_EQ(ping_snmp(&host, &ping), HOST_UP);
  ASSERT_TRUE(strcmp(ping.snmp_status, "0.00") == 0);
  host.snmp_session = NULL;
  ASSERT_INT_EQ(ping_snmp(&host, &ping), HOST_DOWN);
  ASSERT_TRUE(strcmp(ping.snmp_response, "Invalid SNMP Session") == 0);
}

static void test_persisted_host_state(void) {
  host_t host = make_host();
  ping_t ping = {0};
  host.status = HOST_UP;
  host.min_time = 100;
  set.ping_failure_count = set.ping_recovery_count = 2;
  snprintf(ping.ping_response, sizeof(ping.ping_response), "failure");
  snprintf(ping.snmp_response, sizeof(ping.snmp_response), "snmp failure");
  update_host_status(HOST_DOWN, &host, &ping, AVAIL_SNMP_AND_PING);
  ASSERT_INT_EQ(host.status, HOST_UP);
  ASSERT_INT_EQ(host.status_event_count, 1);
  ASSERT_TRUE(strcmp(host.status_last_error, "snmp failure, failure") == 0);
  ASSERT_TRUE(host.status_fail_date[0] != '\0');
  update_host_status(HOST_DOWN, &host, &ping, AVAIL_SNMP_AND_PING);
  ASSERT_INT_EQ(host.status, HOST_DOWN);
  snprintf(ping.ping_status, sizeof(ping.ping_status), "4.00000");
  snprintf(ping.snmp_status, sizeof(ping.snmp_status), "2.00000");
  update_host_status(HOST_UP, &host, &ping, AVAIL_SNMP_AND_PING);
  ASSERT_INT_EQ(host.status, HOST_RECOVERING);
  ASSERT_TRUE(host.cur_time == 3.0 && host.avg_time == 3.0);
  update_host_status(HOST_UP, &host, &ping, AVAIL_SNMP_AND_PING);
  ASSERT_INT_EQ(host.status, HOST_UP);
  ASSERT_INT_EQ(host.status_event_count, 0);
  ASSERT_INT_EQ(host.total_polls, 4);
  ASSERT_INT_EQ(host.failed_polls, 2);
  ASSERT_TRUE(host.availability == 50.0);
  ASSERT_TRUE(host.status_rec_date[0] != '\0');
}

int main(void) {
  test_availability_matrix();
  test_retry_and_format_contract();
  test_snmp_contract();
  test_persisted_host_state();
  return finish_tests("ping baseline behavior");
}
