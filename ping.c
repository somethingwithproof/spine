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
#include <fcntl.h>

/*! \fn int ping_host(host_t *host, ping_t *ping)
 *  \brief ping a host to determine if it is reachable for polling
 *  \param host a pointer to the current host structure
 *  \param ping a pointer to the current hosts ping structure
 *
 *  This function pings a host using the method specified within the system
 *  configuration and then returns the host status to the calling function.
 *
 *  \return HOST_UP if the host is reachable, HOST_DOWN otherwise.
 */
static int ping_network(host_t *host, ping_t *ping) {
	if (host->ping_method == PING_ICMP && !set.icmp_avail) {
		SPINE_LOG(("Device[%i] DEBUG Falling back to UDP Ping Due to SetUID Issues", host->id));
		host->ping_method = PING_UDP;
	}
	if (strstr(host->hostname, "localhost")) {
		STRNCOPY(ping->ping_status, "0.000");
		STRNCOPY(ping->ping_response, "PING: Device does not require ping.");
		return HOST_UP;
	}
	if (get_address_type(host) != 1) {
		if (host->availability_method == AVAIL_PING) {
			STRNCOPY(ping->ping_status, "0.000");
			STRNCOPY(ping->ping_response, "PING: Device is Unknown or is IPV6.  Please use the SNMP ping options only.");
		}
		return HOST_DOWN;
	}
	switch (host->ping_method) {
		case PING_ICMP: return ping_icmp(host, ping);
		case PING_UDP: return ping_udp(host, ping);
		case PING_TCP:
		case PING_TCP_CLOSED: return ping_tcp(host, ping);
		default: return HOST_DOWN;
	}
}

static int ping_snmp_availability(host_t *host, ping_t *ping, int ping_result) {
	if (host->availability_method == AVAIL_SNMP_AND_PING && ping_result != HOST_UP) return HOST_DOWN;
	if (host->availability_method == AVAIL_SNMP_OR_PING && ping_result == HOST_UP) return HOST_UP;
	/* Preserve the configured no-SNMP contract for v1/v2 without a community. */
	if (host->snmp_community[0] == '\0' && host->snmp_version < 3) return HOST_UP;
	double begin = get_time_as_double();
	int result = ping_snmp(host, ping);
	double elapsed = (get_time_as_double() - begin) * 1000.0;
	SPINE_LOG_DEVICE(host->id, POLLER_VERBOSITY_MEDIUM,
		("Device[%i] INFO: SNMP Device %s, Time:%.4f ms", host->id, result == HOST_UP ? "Alive" : "Down", elapsed));
	return result;
}

int ping_host(host_t *host, ping_t *ping) {
	int network_result = HOST_DOWN;
	if (host->availability_method == AVAIL_SNMP_AND_PING ||
		host->availability_method == AVAIL_PING ||
		host->availability_method == AVAIL_SNMP_OR_PING) {
		network_result = ping_network(host, ping);
	}
	switch (host->availability_method) {
		case AVAIL_SNMP_AND_PING:
		case AVAIL_SNMP_OR_PING:
		case AVAIL_SNMP:
		case AVAIL_SNMP_GET_NEXT:
		case AVAIL_SNMP_GET_SYSDESC:
			return ping_snmp_availability(host, ping, network_result);
		case AVAIL_PING: return network_result;
		case AVAIL_NONE: return HOST_UP;
		default: return HOST_DOWN;
	}
}

/*! \fn int ping_snmp(host_t *host, ping_t *ping)
 *  \brief ping a host using snmp sysUptime
 *  \param host a pointer to the current host structure
 *  \param ping a pointer to the current hosts ping structure
 *
 *  This function pings a host using snmp.  It polls sysUptime by default.
 *  It will modify the ping structure to include the specifics of the ping results.
 *
 *  \return HOST_UP if the host is reachable, HOST_DOWN otherwise.
 *
 */
int ping_snmp(host_t *host, ping_t *ping) {
	SPINE_LOG_DEVICE(host->id, POLLER_VERBOSITY_DEBUG, ("Device[%i] DEBUG: Entering SNMP Ping", host->id));
	if (!host->snmp_session) {
		STRNCOPY(ping->snmp_status, "0.00");
		STRNCOPY(ping->snmp_response, "Invalid SNMP Session");
		return HOST_DOWN;
	}
	if (host->snmp_community[0] == '\0' && host->snmp_version != 3) {
		STRNCOPY(ping->snmp_status, "0.00");
		STRNCOPY(ping->snmp_response, "Device does not require SNMP");
		return HOST_UP;
	}
	char oid[32];
	switch (host->availability_method) {
		case AVAIL_SNMP_GET_NEXT: STRNCOPY(oid, ".1.3"); break;
		case AVAIL_SNMP_GET_SYSDESC: STRNCOPY(oid, ".1.3.6.1.2.1.1.1.0"); break;
		default: STRNCOPY(oid, ".1.3.6.1.2.1.1.3.0"); break;
	}
	double begin = get_time_as_double();
	char *result = host->availability_method == AVAIL_SNMP_GET_NEXT ? snmp_getnext(host, oid) : snmp_get(host, oid);
	double elapsed = (get_time_as_double() - begin) * 1000.0;
	SPINE_FREE(result);
	if (host->snmp_status == SNMPERR_SUCCESS || host->snmp_status == SNMPERR_UNKNOWN_OBJID) {
		STRNCOPY(ping->snmp_response, "Device responded to SNMP");
		spine_snprintf(ping->snmp_status, sizeof(ping->snmp_status), "%.5f", elapsed);
		return HOST_UP;
	}
	SPINE_LOG_DEVICE(host->id, POLLER_VERBOSITY_HIGH,
		("Device[%i] SNMP Ping %s", host->id, host->snmp_status == STAT_TIMEOUT ? "Timeout" : "Unknown Error"));
	STRNCOPY(ping->snmp_response, "Device did not respond to SNMP");
	return HOST_DOWN;
}

static int ping_down(ping_t *ping, const char *message);

/* Parse bytes rather than casting an unaligned, possibly short packet. */
bool spine_icmp_reply_matches(const unsigned char *reply, size_t length, uint16_t id, uint16_t sequence) {
	if (reply == NULL || length < 20 || (reply[0] >> 4) != 4 || reply[9] != IPPROTO_ICMP) return FALSE;
	size_t header = (size_t)(reply[0] & 15) * 4;
	if (header < 20 || header > length || length - header < ICMP_HDR_SIZE) return FALSE;
	if (reply[header] != ICMP_ECHOREPLY || reply[header + 1] != 0) return FALSE;
	uint16_t received_id;
	uint16_t received_sequence;
	memcpy(&received_id, reply + header + 4, sizeof(received_id));
	memcpy(&received_sequence, reply + header + 6, sizeof(received_sequence));
	return received_id == id && received_sequence == sequence;
}

/* Privilege ownership is confined to socket creation, including failed
 * attempts; closing an already-owned descriptor needs no privilege change. */
static int ping_icmp_socket(void) {
	#if !(defined(__CYGWIN__) && !defined(SOLAR_PRIV))
	bool change_privilege = hasCaps() != TRUE;
	if (change_privilege) {
		thread_mutex_lock(LOCK_SETEUID);
		if (seteuid(0) == -1) SPINE_LOG_DEBUG(("WARNING: Spine unable to obtain root privileges."));
	}
	#endif
	int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	int socket_error = errno;
	#if !(defined(__CYGWIN__) && !defined(SOLAR_PRIV))
	if (change_privilege) {
		int dropped = seteuid(getuid());
		thread_mutex_unlock(LOCK_SETEUID);
		if (dropped == -1) {
			if (fd >= 0) close(fd);
			set.exit_code = EXIT_FAILURE;
			die("ERROR: Spine unable to drop from root to local user");
		}
	}
	#endif
	errno = socket_error;
	return fd;
}

static int ping_icmp_open(void) {
	for (int attempt = 0; attempt < 5; attempt++) {
		int fd = ping_icmp_socket();
		if (fd >= 0) {
			int flags = fcntl(fd, F_GETFL, 0);
			if (flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0) return fd;
			close(fd);
			return -1;
		}
		spine_sleep_usec(500000);
	}
	return -1;
}

static int ping_icmp_response(int fd, const struct sockaddr_in *target, const struct icmp *request, double deadline) {
	unsigned char reply[BUFSIZE];
	for (;;) {
		int ready = spine_wait_readable(fd, deadline);
		if (ready <= 0) return ready;
		struct sockaddr_in source = {0};
		socklen_t length = sizeof(source);
		ssize_t received = recvfrom(fd, reply, sizeof(reply), 0, (struct sockaddr *)&source, &length);
		if (received < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
			return -1;
		}
		if (length >= sizeof(source) && source.sin_addr.s_addr == target->sin_addr.s_addr &&
			spine_icmp_reply_matches(reply, (size_t)received, request->icmp_id, request->icmp_seq)) return 1;
	}
}

int ping_icmp(const host_t *host, ping_t *ping) {
	SPINE_LOG_DEVICE(host->id, POLLER_VERBOSITY_DEBUG, ("Device[%i] DEBUG: Entering ICMP Ping", host->id));
	if (host->hostname[0] == '\0') return ping_down(ping, "ICMP: Destination address not specified");
	struct sockaddr_in target = {0};
	if (!init_sockaddr(&target, host->hostname, 7)) return ping_down(ping, "ICMP: Destination hostname invalid");
	if (host->ping_timeout <= 0 || host->ping_retries < 0) return ping_down(ping, "ICMP: Ping timed out");
	int fd = ping_icmp_open();
	if (fd < 0) return ping_down(ping, "ICMP: Ping unable to create ICMP Socket");
	static const char payload[] = "cacti-monitoring-system";
	union {
		struct icmp alignment;
		unsigned char bytes[ICMP_HDR_SIZE + sizeof(payload) - 1];
	} packet = {0};
	struct icmp *request = &packet.alignment;
	request->icmp_type = ICMP_ECHO;
	request->icmp_id = htons((uint16_t)((unsigned int)getpid() & 65535));
	static unsigned int sequence;
	thread_mutex_lock(LOCK_GHBN);
	request->icmp_seq = htons((uint16_t)(sequence++ & 65535));
	thread_mutex_unlock(LOCK_GHBN);
	memcpy(packet.bytes + ICMP_HDR_SIZE, payload, sizeof(payload) - 1);
	request->icmp_cksum = get_checksum(packet.bytes, sizeof(packet.bytes));
	double begin = spine_monotonic_time();
	for (unsigned int attempt = 0; ; attempt++) {
		double deadline = spine_monotonic_time() + (double)host->ping_timeout / 1000;
		ssize_t sent;
		do {
			sent = sendto(fd, packet.bytes, sizeof(packet.bytes), 0, (struct sockaddr *)&target, sizeof(target));
		} while (sent < 0 && errno == EINTR && spine_monotonic_time() < deadline);
		int result = sent == (ssize_t)sizeof(packet.bytes) ? ping_icmp_response(fd, &target, request, deadline) : -1;
		if (result > 0) {
			double elapsed = (spine_monotonic_time() - begin) * 1000;
			SPINE_LOG_DEVICE(host->id, POLLER_VERBOSITY_MEDIUM, ("Device[%i] INFO: ICMP Device Alive, Try Count:%u, Time:%.4f ms", host->id, attempt + 1, elapsed));
			strncopy(ping->ping_response, "ICMP: Device is Alive", SMALL_BUFSIZE);
			snprintf(ping->ping_status, 50, "%.5f", elapsed);
			close(fd);
			return HOST_UP;
		}
		if (attempt >= (unsigned int)host->ping_retries) {
			close(fd);
			return ping_down(ping, "ICMP: Ping timed out");
		}
	}
}

/*! \fn int ping_udp(const host_t *host, ping_t *ping)
 *  \brief ping a host using an UDP datagram
 *  \param host a pointer to the current host structure
 *  \param ping a pointer to the current hosts ping structure
 *
 *  This function pings a host using UDP.  The UDP datagram contains a marker
 *  to the "Cacti" application so that firewall's can be configured to allow.
 *  It will modify the ping structure to include the specifics of the ping results.
 *
 *  \return HOST_UP if the host is reachable, HOST_DOWN otherwise.
 *
 */
static int ping_down(ping_t *ping, const char *message) {
	snprintf(ping->ping_status, 50, "down");
	strncopy(ping->ping_response, message, SMALL_BUFSIZE);
	return HOST_DOWN;
}

/* UDP reachability is established by the existing ICMP port-error contract;
 * receiving an application datagram alone does not establish that result. */
static int ping_udp_response(int fd, double deadline) {
	char response[BUFSIZE];
	for (;;) {
		int ready = spine_wait_readable(fd, deadline);
		if (ready <= 0) return ready;
		ssize_t received = recv(fd, response, sizeof(response), 0);
		if (received >= 0) continue;
		if (errno == EHOSTUNREACH || errno == ECONNRESET || errno == ECONNREFUSED) return 1;
		if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) return -1;
	}
}

int ping_udp(const host_t *host, ping_t *ping) {
	SPINE_LOG_DEVICE(host->id, POLLER_VERBOSITY_DEBUG, ("Device[%i] DEBUG: Entering UDP Ping", host->id));
	if (host->hostname[0] == '\0') return ping_down(ping, "UDP: Destination address invalid or unable to create socket");
	struct sockaddr_in servername = {0};
	if (!init_sockaddr(&servername, host->hostname, host->ping_port)) return ping_down(ping, "UDP: Destination hostname invalid");
	if (host->ping_timeout <= 0 || host->ping_retries < 0) return ping_down(ping, "UDP: Ping timed out");
	int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd < 0) return ping_down(ping, "UDP: Destination address invalid or unable to create socket");
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
		connect(fd, (struct sockaddr *)&servername, sizeof(servername)) < 0) {
		close(fd);
		return ping_down(ping, "UDP: Cannot connect to host");
	}
	static const char request[] = "cacti-monitoring-system";
	double begin = spine_monotonic_time();
	for (unsigned int attempt = 0; ; attempt++) {
		double deadline = spine_monotonic_time() + (double)host->ping_timeout / 1000;
		ssize_t sent;
		do {
			sent = send(fd, request, sizeof(request) - 1, 0);
		} while (sent < 0 && errno == EINTR && spine_monotonic_time() < deadline);
		int result = sent == (ssize_t)(sizeof(request) - 1) ? ping_udp_response(fd, deadline) : -1;
		double elapsed = (spine_monotonic_time() - begin) * 1000;
		if (result > 0) {
			SPINE_LOG_DEVICE(host->id, POLLER_VERBOSITY_MEDIUM, ("Device[%i] INFO: UDP Device Alive, Try Count:%u, Time:%.4f ms", host->id, attempt + 1, elapsed));
			strncopy(ping->ping_response, "UDP: Device is Alive", SMALL_BUFSIZE);
			snprintf(ping->ping_status, 50, "%.5f", elapsed);
			close(fd);
			return HOST_UP;
		}
		if (result < 0 || attempt >= (unsigned int)host->ping_retries) {
			close(fd);
			return ping_down(ping, result < 0 ? "UDP: Device is Down" : "UDP: Ping timed out");
		}
		SPINE_LOG_DEVICE(host->id, POLLER_VERBOSITY_DEBUG, ("Device[%i] DEBUG: UDP Timeout, Try Count:%u, Time:%.4f ms", host->id, attempt + 1, elapsed));
	}
}


/*! \fn int ping_tcp(const host_t *host, ping_t *ping)
 *  \brief ping a host using an TCP syn
 *  \param host a pointer to the current host structure
 *  \param ping a pointer to the current hosts ping structure
 *
 *  This function pings a host using TCP.  The TCP socket contains a marker
 *  to the "Cacti" application so that firewall's can be configured to allow.
 *  It will modify the ping structure to include the specifics of the ping results.
 *
 *  \return HOST_UP if the host is reachable, HOST_DOWN otherwise.
 *
 */
/* Each retry owns a new nonblocking socket; a failed connect socket must not
 * be reused. SO_ERROR distinguishes completion from refused/timed-out peers. */
static int ping_tcp_complete(int fd, double deadline) {
	int ready = spine_wait_writable(fd, deadline);
	if (ready <= 0) return ready == 0 ? ETIMEDOUT : errno;
	int error = 0;
	socklen_t length = sizeof(error);
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0) return errno;
	return error;
}

static int ping_tcp_attempt(int fd, const struct sockaddr_in *address, double deadline) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return errno;
	if (connect(fd, (const struct sockaddr *)address, sizeof(*address)) == 0) return 0;
	int error = errno;
	if (error != EINPROGRESS && error != EINTR) return error;
	return ping_tcp_complete(fd, deadline);
}

static int ping_tcp_connect(const struct sockaddr_in *address, double deadline) {
	int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) return errno;
	int error = ping_tcp_attempt(fd, address, deadline);
	close(fd);
	return error;
}

int ping_tcp(const host_t *host, ping_t *ping) {
	SPINE_LOG_DEVICE(host->id, POLLER_VERBOSITY_DEBUG, ("Device[%i] DEBUG: Entering TCP Ping", host->id));
	if (host->hostname[0] == '\0') return ping_down(ping, "TCP: Destination address invalid or unable to create socket");
	struct sockaddr_in address = {0};
	if (!init_sockaddr(&address, host->hostname, host->ping_port)) return ping_down(ping, "TCP: Destination hostname invalid");
	if (host->ping_timeout <= 0 || host->ping_retries < 0) return ping_down(ping, "TCP: Cannot connect to host");
	double begin = spine_monotonic_time();
	for (unsigned int attempt = 0; ; attempt++) {
		double deadline = spine_monotonic_time() + (double)host->ping_timeout / 1000;
		int error = ping_tcp_connect(&address, deadline);
		if (error == 0 || (error == ECONNREFUSED && host->ping_method == PING_TCP_CLOSED)) {
			double elapsed = (spine_monotonic_time() - begin) * 1000;
			SPINE_LOG_DEVICE(host->id, POLLER_VERBOSITY_MEDIUM, ("Device[%i] INFO: TCP Device Alive, Try Count:%u, Time:%.4f ms", host->id, attempt + 1, elapsed));
			strncopy(ping->ping_response, "TCP: Device is Alive", SMALL_BUFSIZE);
			snprintf(ping->ping_status, 50, "%.5f", elapsed);
			return HOST_UP;
		}
		/* Cygwin historically makes one attempt only. */
		#if defined(__CYGWIN__)
		return ping_down(ping, "TCP: Cannot connect to host");
		#else
		if (attempt >= (unsigned int)host->ping_retries) return ping_down(ping, "TCP: Cannot connect to host");
		#endif
	}
}

/*! \fn int get_address_type(host_t *host)
 *  \brief determines using getaddrinfo the iptype and returns the iptype
 *
 *  \return 1 - IPv4, 2 - IPv6, 0 - Unknown
 */
int get_address_type(host_t *host) {
	struct addrinfo hints;
	struct addrinfo *res_list;
	char addrstr[255];
	const void *ptr = NULL;
	int addr_found = FALSE;

	memset(&hints, 0, sizeof(hints));

	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags    = AI_CANONNAME | AI_ADDRCONFIG;
	int error;

	if ((error = getaddrinfo(host->hostname, NULL, &hints, &res_list)) != 0) {
		SPINE_LOG(("WARNING: Unable to determine address info for %s (%s)", host->hostname, gai_strerror(error)));
		return SPINE_NONE;
	}

	for (struct addrinfo *res = res_list; res != NULL; res = res->ai_next) {
		inet_ntop(res->ai_family, res->ai_addr->sa_data, addrstr, 100);

		switch(res->ai_family) {
			case AF_INET:
				ptr = &((struct sockaddr_in *) res->ai_addr)->sin_addr;
				addr_found = TRUE;
				break;
			case AF_INET6:
				ptr = &((struct sockaddr_in6 *) res->ai_addr)->sin6_addr;
				addr_found = TRUE;
				break;
			default:
				continue;
		}

		inet_ntop(res->ai_family, ptr, addrstr, 100);

		SPINE_LOG_HIGH(("Device[%d] IPv%d address %s (%s)", host->id, res->ai_family == PF_INET6 ? 6:4, addrstr, res->ai_canonname));

		if (res->ai_family != PF_INET6) {
			freeaddrinfo(res_list);

			return SPINE_IPV4;
		}
	}

	freeaddrinfo(res_list);

	if (addr_found) {
		return SPINE_IPV6;
	} else {
		return SPINE_NONE;
	}
}

/*! \fn int init_sockaddr(struct sockaddr_in *name, const char *hostname, int port)
 *  \brief converts a hostname to an internet address
 *
 *  \return TRUE if successful, FALSE otherwise.
 *
 */
int init_sockaddr(struct sockaddr_in *name, const char *hostname, int port) {
	if (port < 0 || port > 65535) return FALSE;
	struct addrinfo hints = {0};
	struct addrinfo *hostinfo = NULL;
	hints.ai_family = AF_INET;
	hints.ai_flags = AI_CANONNAME | AI_ADDRCONFIG;
	for (int attempt = 0; ; attempt++) {
		int status = getaddrinfo(hostname, NULL, &hints, &hostinfo);
		if (status == 0) break;
		if (status == EAI_AGAIN && attempt < 3) {
			SPINE_LOG(("WARNING: Temporary DNS error for host %s (%s), retrying", hostname, gai_strerror(status)));
			spine_sleep_usec(50000);
			continue;
		}
		SPINE_LOG(("WARNING: Error resolving host %s (%s)", hostname, gai_strerror(status)));
		return FALSE;
	}
	if (hostinfo == NULL) {
		SPINE_LOG(("WARNING: Unknown host %s", hostname));
		return FALSE;
	}
	name->sin_family = AF_INET;
	name->sin_addr = ((struct sockaddr_in *)hostinfo->ai_addr)->sin_addr;
	name->sin_port = htons((unsigned short)port);
	freeaddrinfo(hostinfo);
	return TRUE;
}

/*! \brief Parse a device hostname with an optional transport and port. */
name_t *get_namebyhost(const char *hostname, name_t *name) {
	if (name == NULL) {
		name = calloc(1, sizeof(*name));
		if (name == NULL) die("ERROR: Fatal malloc error: ping.c get_namebyhost->name");
	}
	/* IPv6 transport addresses are passed intact to the address resolver. */
	if (strchr(hostname, '[') != NULL || strstr(hostname, "::") != NULL || char_count(hostname, ':') > 2) {
		strncopy(name->hostname, hostname, sizeof(name->hostname));
		return name;
	}
	char *copy = strdup(hostname);
	if (copy == NULL) die("ERROR: Fatal malloc error: ping.c get_namebyhost->stack");
	char *saveptr = NULL;
	const char *token = strtok_r(copy, ":", &saveptr);
	static const char *const methods[] = {"TCP", "UDP", "TCP6", "UDP6"};
	if (token != NULL) {
		for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
			if (strcasecmp(token, methods[i]) == 0) {
				name->method = (int)i + 1;
				token = strtok_r(NULL, ":", &saveptr);
				break;
			}
		}
	}
	strncopy(name->hostname, token != NULL ? token : hostname, sizeof(name->hostname));
	token = strtok_r(NULL, ":", &saveptr);
	if (token != NULL) name->port = atoi(token);
	free(copy);
	return name;
}

/*! \fn unsigned short int get_checksum(void* buf, int len)
 *  \brief calculates a 16bit checksum of a packet buffer
 *  \param buf the input buffer to calculate the checksum of
 *  \param len the size of the input buffer
 *
 *  \return 16bit checksum of an input buffer of size len.
 *
 */
unsigned short int get_checksum(void* buf, int len) {
	int      nleft = len;
	int32_t  sum   = 0;
	unsigned short int answer;
	unsigned short int* w = (unsigned short int*)buf;
	unsigned short int odd_byte = 0;

	while (nleft > 1) {
		sum += *w++;
		nleft -= 2;
	}

	if (nleft == 1) {
   		*(unsigned char*)(&odd_byte) = *(unsigned char*)w;
   		sum += odd_byte;
	}

	sum    = (sum >> 16) + (sum & 0xffff);
	sum   += (sum >> 16);
	answer = (unsigned short int)((unsigned int)~sum & 0xffff); /* checksum truncation is intentional */

	return answer;
}

/*! \fn void update_host_status(int status, host_t *host, ping_t *ping, int availability_method)
 *  \brief update the host table in Cacti with the result of the ping of the host.
 *  \param status the current poll status of the host, either HOST_UP, or HOST_DOWN
 *  \param host a pointer to the current host structure
 *  \param ping a pointer to the current hosts ping structure
 *  \param availability_method the method that was used to poll the host
 *
 *  This function will determine if the host is UP, DOWN, or RECOVERING based upon
 *  the ping result and it's current status.  It will update the Cacti database
 *  with the calculated status.
 *
 */
void update_host_status(int status, host_t *host, ping_t *ping, int availability_method) {
	int    issue_log_message = FALSE;
	double ping_time;
 	double hundred_percent = 100.00;
	char   current_date[40];

	snprintf(current_date, 40, "%lu", time(NULL));

	/* host is down */
	if (status == HOST_DOWN) {
		/* update total polls, failed polls and availability */
		host->failed_polls = host->failed_polls + 1;
		host->total_polls = host->total_polls + 1;
		host->availability = hundred_percent * (host->total_polls - host->failed_polls) / host->total_polls;

		/*determine the error message to display */
		switch (availability_method) {
		case AVAIL_SNMP_OR_PING:
		case AVAIL_SNMP_AND_PING:
			if (strlen(host->snmp_community) == 0 && host->snmp_version < 3) {
				snprintf(host->status_last_error, BUFSIZE * 2 + 1, "%s", ping->ping_response);
			} else {
				snprintf(host->status_last_error, BUFSIZE * 2 + 1, "%s, %s", ping->snmp_response, ping->ping_response);
			}
			break;
		case AVAIL_SNMP:
			if (strlen(host->snmp_community) == 0 && host->snmp_version < 3) {
				snprintf(host->status_last_error, BUFSIZE * 2 + 1, "%s", "Device does not require SNMP");
			} else {
				snprintf(host->status_last_error, BUFSIZE * 2 + 1, "%s", ping->snmp_response);
			}
			break;
		default:
			snprintf(host->status_last_error, BUFSIZE * 2 + 1, "%s", ping->ping_response);
		}

		/* determine if to send an alert and update remainder of statistics */
		if (host->status == HOST_UP) {
			/* increment the event failure count */
			host->status_event_count++;

			/* if it's time to issue an error message, indicate so */
			if (host->status_event_count >= set.ping_failure_count) {
				/* host is now down, flag it that way */
				host->status = HOST_DOWN;

				issue_log_message = TRUE;

				/* update the failure date only if the failure count is 1 */
				if (set.ping_failure_count == 1) {
					snprintf(host->status_fail_date, 40, "%s", current_date);
				}
			} else {
				/* host down for the first time, set event date */
				if (host->status_event_count == 1) {
					snprintf(host->status_fail_date, 40, "%s", current_date);
				}
			}
		} else if (host->status == HOST_RECOVERING) {
			/* host is recovering, put back in failed state */
			host->status_event_count = 1;
			host->status = HOST_DOWN;
		} else if (host->status == HOST_UNKNOWN) {
			/* host was unknown and now is down */
			host->status = HOST_DOWN;
			host->status_event_count = 0;
		} else {
			host->status_event_count++;
		}
	} else {
		/* host is up!! */

		/* update total polls and availability */
		host->total_polls = host->total_polls + 1;
		host->availability = hundred_percent * (host->total_polls - host->failed_polls) / host->total_polls;

		/* determine the ping statistic to set and do so */
		if (availability_method == AVAIL_SNMP_AND_PING) {
			if (strlen(host->snmp_community) == 0 && host->snmp_version < 3) {
				ping_time = atof(ping->ping_status);
			} else {
				/* calculate the average of the two times */
				ping_time = (atof(ping->snmp_status) + atof(ping->ping_status)) / 2;
			}
		} else if (availability_method == AVAIL_SNMP) {
			if (strlen(host->snmp_community) == 0 && host->snmp_version < 3) {
				ping_time = 0.000;
			} else {
				ping_time = atof(ping->snmp_status);
			}
		} else if (availability_method == AVAIL_NONE) {
			ping_time = 0.000;
		} else {
			ping_time = atof(ping->ping_status);
		}

		/* update times as required */
		host->cur_time = ping_time;

		/* maximum time */
		if (ping_time > host->max_time)
			host->max_time = ping_time;

		/* minimum time */
		if (ping_time < host->min_time)
			host->min_time = ping_time;

		/* average time */
		host->avg_time = (((host->total_polls-1-host->failed_polls)
			* host->avg_time) + ping_time) / (host->total_polls-host->failed_polls);

		/* the host was down, now it's recovering */
		if ((host->status == HOST_DOWN) || (host->status == HOST_RECOVERING)) {
			/* just up, change to recovering */
			if (host->status == HOST_DOWN) {
				host->status = HOST_RECOVERING;
				host->status_event_count = 1;
			} else {
				host->status_event_count++;
			}

			/* if it's time to issue a recovery message, indicate so */
			if (host->status_event_count >= set.ping_recovery_count) {
				/* host is up, flag it that way */
				host->status = HOST_UP;

				issue_log_message = TRUE;

				/* update the recovery date only if the recovery count is 1 */
				if (set.ping_recovery_count == 1) {
					snprintf(host->status_rec_date, 40, "%s", current_date);
				}

				/* reset the event counter */
				host->status_event_count = 0;
			} else {
				/* host recovering for the first time, set event date */
				if (host->status_event_count == 1) {
					snprintf(host->status_rec_date, 40, "%s", current_date);
				}
			}
		} else if (host->status_event_count > 0) {
			/* host was unknown and now is up */
			host->status = HOST_UP;
			host->status_event_count = 0;
		} else {
			/* host was unknown and now is up */
			host->status = HOST_UP;
			host->status_event_count = 0;
		}
	}

	/* if the user wants a flood of information then flood them */
	if (set.log_level >= POLLER_VERBOSITY_HIGH) {
		if ((host->status == HOST_UP) || (host->status == HOST_RECOVERING)) {
			/* log ping result if we are to use a ping for reachability testing */
			if (availability_method == AVAIL_SNMP_AND_PING) {
				SPINE_LOG_DEVICE(host->id, POLLER_VERBOSITY_HIGH, ("Device[%i] PING Result: %s", host->id, ping->ping_response));
				SPINE_LOG_DEVICE(host->id, POLLER_VERBOSITY_HIGH, ("Device[%i] SNMP Result: %s", host->id, ping->snmp_response));
			} else if (availability_method == AVAIL_SNMP_OR_PING) {
				SPINE_LOG_DEVICE(host->id, POLLER_VERBOSITY_HIGH, ("Device[%i] PING Result: %s", host->id, ping->ping_response));
				SPINE_LOG_DEVICE(host->id, POLLER_VERBOSITY_HIGH, ("Device[%i] SNMP Result: %s", host->id, ping->snmp_response));
			} else if (availability_method == AVAIL_SNMP) {
				if ((strlen(host->snmp_community) == 0) && (host->snmp_version < 3)) {
					SPINE_LOG_DEVICE(host->id, POLLER_VERBOSITY_HIGH, ("Device[%i] SNMP Result: Device does not require SNMP", host->id));
				} else {
					SPINE_LOG_DEVICE(host->id, POLLER_VERBOSITY_HIGH, ("Device[%i] SNMP Result: %s", host->id, ping->snmp_response));
				}
			} else if (availability_method == AVAIL_NONE) {
				SPINE_LOG_DEVICE(host->id, POLLER_VERBOSITY_HIGH, ("Device[%i] No Device Availability Method Selected", host->id));
			} else {
				SPINE_LOG_DEVICE(host->id, POLLER_VERBOSITY_HIGH, ("Device[%i] PING: Result %s", host->id, ping->ping_response));
			}
		} else {
			if (availability_method == AVAIL_SNMP_AND_PING) {
				SPINE_LOG_DEVICE(host->id, POLLER_VERBOSITY_HIGH, ("Device[%i] PING Result: %s", host->id, ping->ping_response));
				SPINE_LOG_DEVICE(host->id, POLLER_VERBOSITY_HIGH, ("Device[%i] SNMP Result: %s", host->id, ping->snmp_response));
			} else if (availability_method == AVAIL_SNMP) {
				SPINE_LOG_DEVICE(host->id, POLLER_VERBOSITY_HIGH, ("Device[%i] SNMP Result: %s", host->id, ping->snmp_response));
			} else if (availability_method == AVAIL_NONE) {
				SPINE_LOG_DEVICE(host->id, POLLER_VERBOSITY_HIGH, ("Device[%i] No Device Availability Method Selected", host->id));
			} else {
				SPINE_LOG_DEVICE(host->id, POLLER_VERBOSITY_HIGH, ("Device[%i] PING Result: %s", host->id, ping->ping_response));
			}
		}
	}

	/* if there is supposed to be an event generated, do it */
	if (issue_log_message) {
		if (host->status == HOST_DOWN) {
			SPINE_LOG(("Device[%i] Hostname[%s] ERROR: HOST EVENT: Device is DOWN Message: %s", host->id, host->hostname, host->status_last_error));
		} else {
			SPINE_LOG(("Device[%i] Hostname[%s] NOTICE: HOST EVENT: Device Returned from DOWN State", host->id, host->hostname));
		}
	}
}
