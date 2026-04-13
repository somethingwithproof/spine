#ifndef SPINE_COMMON_H
#define SPINE_COMMON_H 1

// clang-format off
#undef PACKAGE_NAME
#undef PACKAGE_VERSION
#undef PACKAGE_BUGREPORT
#undef PACKAGE_STRING
#undef PACKAGE_TARNAME

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>
// clang-format on

#include "config/config.h"

#ifdef __CYGWIN__
/* We use a Unix API, so pretend it's not Windows */
#undef WIN
#undef WIN32
#undef _WIN
#undef _WIN32
#undef _WIN64
#undef __WIN__
#undef __WIN32__
#define HAVE_ERRNO_AS_DEFINE

/* Cygwin supports only 64 open file descriptors, let's increase it a bit. */
#define FD_SETSIZE 512
#endif /* __CYGWIN__ */

#define _THREAD_SAFE
#define _PTHREADS
#define _P __P

#ifndef _REENTRANT
#define _REENTRANT
#endif

#ifndef _LIBC_REENTRANT
#define _LIBC_REENTRANT
#endif

#define PTHREAD_MUTEXATTR_DEFAULT ((pthread_mutexattr_t *)0)

#if STDC_HEADERS
#include <stdlib.h>
#include <string.h>
#elif HAVE_STRINGS_H
#include <strings.h>
#endif /*STDC_HEADERS*/

#if HAVE_UNISTD_H
#include <sys/types.h>
#include <unistd.h>
#endif

#include "spine_sem.h"
#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <mysql.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>

#if HAVE_STDINT_H
#include <stdint.h>
#endif

#if HAVE_NETINET_IN_H
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#ifndef __CYGWIN__
#include <netinet/icmp6.h>
#endif
#include <netinet/ip_icmp.h>
#endif

#if TIME_WITH_SYS_TIME
#include <sys/time.h>
#include <time.h>
#else
#if HAVE_SYS_TIME_H
#include <sys/time.h>
#else
#include <time.h>
#endif
#endif

#ifndef HAVE_LIBPTHREAD
#define HAVE_LIBPTHREAD 0
#else
#include <pthread.h>
#endif

#ifdef SOLAR_PRIV
#include <priv.h>
#endif

#ifdef HAVE_LCAP
#include <grp.h>
#include <sys/capability.h>
#include <sys/prctl.h>
#endif

#include "uthash.h"

#endif /* SPINE_COMMON_H */
