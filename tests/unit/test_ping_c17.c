/* Exercise the actual ping.c implementation, including private helpers.
 * Resolver stubs make IPv6 sizing checks independent of local networking. */
#include "common.h"
#include "spine.h"
#include "test_platform_helpers.h"

static struct sockaddr_in6 resolved_address;
static struct addrinfo resolved_info;
static int resolver_flags;
static int resolver_frees;

static int test_getaddrinfo(const char *node, const char *service,
                            const struct addrinfo *hints,
                            struct addrinfo **result) {
  (void)node;
  (void)service;
  resolver_flags = hints->ai_flags;
  resolved_info = (struct addrinfo){
      .ai_family = AF_INET6,
      .ai_addrlen = sizeof(resolved_address),
      .ai_addr = (struct sockaddr *)&resolved_address,
  };
  *result = &resolved_info;
  return 0;
}

static void test_freeaddrinfo(struct addrinfo *info) {
  ASSERT_TRUE(info == &resolved_info);
  resolver_frees++;
}

static unsigned char sent_packet[128];
static size_t sent_length;

static int test_socket(int family, int type, int protocol) {
  (void)family;
  (void)type;
  (void)protocol;
  return 7;
}
static ssize_t test_sendto(int fd, const void *buf, size_t len, int flags,
                           const struct sockaddr *address,
                           socklen_t address_len) {
  (void)fd;
  (void)flags;
  (void)address;
  (void)address_len;
  ASSERT_TRUE(len <= sizeof(sent_packet));
  if (len > sizeof(sent_packet))
    return -1;
  memcpy(sent_packet, buf, len);
  sent_length = len;
  return (ssize_t)len;
}
static int test_select(int nfds, fd_set *readfds, fd_set *writefds,
                       fd_set *exceptfds, struct timeval *timeout) {
  (void)nfds;
  (void)readfds;
  (void)writefds;
  (void)exceptfds;
  (void)timeout;
  return 0;
}
static int test_close(int fd) {
  ASSERT_INT_EQ(fd, 7);
  return 0;
}
static int test_setsockopt(int fd, int level, int option, const void *value,
                           socklen_t length) {
  (void)fd;
  (void)level;
  (void)option;
  (void)value;
  (void)length;
  return 0;
}

#define socket test_socket
#define sendto test_sendto
#define select test_select
#define close test_close
#define setsockopt test_setsockopt
#define getaddrinfo test_getaddrinfo
#define freeaddrinfo test_freeaddrinfo
#include "../../src/ping.c"
#undef getaddrinfo
#undef freeaddrinfo
#undef socket
#undef sendto
#undef select
#undef close
#undef setsockopt

/* These unrelated runtime paths are linked but never invoked by this test. */
char *snmp_get(host_t *host, const char *oid) {
  (void)host;
  (void)oid;
  return NULL;
}
char *snmp_getnext(host_t *host, const char *oid) {
  return snmp_get(host, oid);
}

static void test_resolver_capacity(void) {
  struct {
    struct sockaddr_in6 address;
    unsigned char guard[32];
  } target;
  memset(&target, 0xa5, sizeof(target));
  resolved_address = (struct sockaddr_in6){.sin6_family = AF_INET6};
  resolved_address.sin6_scope_id = 42;
  socklen_t length = 0;
  ASSERT_INT_EQ(resolve_sockaddr(&target.address, sizeof(target.address),
                                 &length, AF_INET6, "::1", 7),
                TRUE);
  ASSERT_INT_EQ(length, sizeof(target.address));
  ASSERT_INT_EQ(target.address.sin6_scope_id, 42);
  ASSERT_TRUE((resolver_flags & AI_NUMERICHOST) != 0);
  for (size_t i = 0; i < sizeof(target.guard); i++) {
    ASSERT_INT_EQ(target.guard[i], 0xa5);
  }
  unsigned char small[4] = {0};
  length = 99;
  ASSERT_INT_EQ(
      resolve_sockaddr(small, sizeof(small), &length, AF_INET6, "::1", 7),
      FALSE);
  ASSERT_INT_EQ(length, 0);
  ASSERT_INT_EQ(resolver_frees, 2);
}

static uint16_t reference_checksum(const unsigned char *bytes, size_t length) {
  uint64_t sum = 0;
  for (size_t i = 0; i < length; i += 2) {
    sum += (uint16_t)bytes[i] << 8;
    if (i + 1 < length)
      sum += bytes[i + 1];
  }
  while (sum >> 16)
    sum = (sum & UINT16_MAX) + (sum >> 16);
  return (uint16_t)~sum;
}

static void test_checksums(void) {
  unsigned char bytes[65536];
  memset(bytes, 0xff, sizeof(bytes));
  bytes[1] = 0x12;
  bytes[2] = 0x34;
  const size_t lengths[] = {0, 1, 2, 3, 24, 65535};
  for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
    const size_t length = lengths[i];
    ASSERT_INT_EQ(ntohs(get_checksum(bytes + 1, (int)length)),
                  reference_checksum(bytes + 1, length));
  }
}

static void test_parser(void) {
  char hostname[sizeof(((name_t *)0)->hostname) * 2];
  memset(hostname, 'a', sizeof(hostname) - 1);
  hostname[sizeof(hostname) - 1] = '\0';
  name_t *name = get_namebyhost(hostname, NULL);
  ASSERT_INT_EQ(strlen(name->hostname), sizeof(name->hostname) - 1);
  free(name);
  name = get_namebyhost("TCP:example.test:443", NULL);
  ASSERT_TRUE(strcmp(name->hostname, "example.test") == 0);
  ASSERT_INT_EQ(name->method, 1);
  ASSERT_INT_EQ(name->port, 443);
  free(name);
  name = get_namebyhost("2001:db8::1", NULL);
  ASSERT_TRUE(strcmp(name->hostname, "2001:db8::1") == 0);
  free(name);
  char first[] = "TCP::one:80";
  char second[] = "UDP:two:53";
  char *a = first;
  char *b = second;
  ASSERT_TRUE(strcmp(ping_next_token(&a), "TCP") == 0);
  ASSERT_TRUE(strcmp(ping_next_token(&b), "UDP") == 0);
  ASSERT_TRUE(strcmp(ping_next_token(&a), "one") == 0);
  ASSERT_TRUE(strcmp(ping_next_token(&b), "two") == 0);
  ASSERT_TRUE(strcmp(ping_next_token(&a), "80") == 0);
  ASSERT_TRUE(ping_next_token(&a) == NULL);
}

static void test_sequence(void) {
  ping_sequence_t sequence = UINT16_MAX;
  ASSERT_INT_EQ(ping_sequence_next(&sequence), UINT16_MAX);
  ASSERT_INT_EQ(ping_sequence_next(&sequence), 0);
  ASSERT_INT_EQ(ping_sequence_next(&sequence), 1);
}

#ifndef _WIN32
static void test_packet_composition(void) {
  spine_icmp_result_t result;
  /* A NULL or zero-length payload consistently selects the signature. */
  ASSERT_INT_EQ(ping_icmp_v4_posix_numeric("127.0.0.1", 1000, NULL, 1, &result),
                0);
  ASSERT_INT_EQ(result.status, SPINE_ICMP_TIMEOUT);
  ASSERT_INT_EQ(sent_length, ICMP_HDR_SIZE + sizeof(spine_ping_payload_t));
  ASSERT_INT_EQ(sent_packet[0], ICMP_ECHO);
  ASSERT_INT_EQ(reference_checksum(sent_packet, sent_length), 0);
  ASSERT_INT_EQ(spine_ping_validate_payload(sent_packet + ICMP_HDR_SIZE,
                                            sent_length - ICMP_HDR_SIZE, 0),
                1);
  const unsigned char payload[] = {1, 2, 3};
  ASSERT_INT_EQ(ping_icmp_v4_posix_numeric("127.0.0.1", 1000, payload,
                                           sizeof(payload), &result),
                0);
  ASSERT_INT_EQ(sent_length, ICMP_HDR_SIZE + sizeof(payload));
  ASSERT_INT_EQ(reference_checksum(sent_packet, sent_length), 0);
  ASSERT_TRUE(memcmp(sent_packet + ICMP_HDR_SIZE, payload, sizeof(payload)) ==
              0);
  ASSERT_INT_EQ(ping_icmp_v6_posix_numeric("::1", 1000, payload, 0, &result),
                0);
  ASSERT_INT_EQ(sent_length,
                sizeof(struct icmp6_hdr) + sizeof(spine_ping_payload_t));
  ASSERT_INT_EQ(sent_packet[0], ICMP6_ECHO_REQUEST);
  ASSERT_INT_EQ(
      spine_ping_validate_payload(sent_packet + sizeof(struct icmp6_hdr),
                                  sent_length - sizeof(struct icmp6_hdr), 0),
      1);
}

static void test_payload_limits(void) {
  spine_icmp_result_t result;
  const unsigned char byte = 1;
  ASSERT_INT_EQ(
      ping_icmp_v4_posix_numeric("127.0.0.1", 1, &byte, SIZE_MAX, &result), -1);
  ASSERT_INT_EQ(result.system_errno, EMSGSIZE);
  ASSERT_INT_EQ(result.status, SPINE_ICMP_ERROR);
  ASSERT_INT_EQ(ping_icmp_v6_posix_numeric("::1", 1, &byte, SIZE_MAX, &result),
                -1);
  ASSERT_INT_EQ(result.system_errno, EMSGSIZE);
}
#endif

int main(void) {
  test_resolver_capacity();
  test_checksums();
  test_parser();
  test_sequence();
#ifndef _WIN32
  test_payload_limits();
  test_packet_composition();
#endif
  return finish_tests("ping C17 regression tests");
}
