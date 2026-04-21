/*
 ex: set tabstop=4 shiftwidth=4 autoindent:
 +-------------------------------------------------------------------------+
 | Copyright (C) 2004-2026 The Cacti Group                                 |
 |                                                                         |
 | This program is free software; you can redistribute it and/or           |
 | modify it under the terms of the GNU Lesser General Public              |
 | License as published by the Free Software Foundation; either            |
 | version 2.1 of the License, or (at your option) any later version.      |
 |                                                                         |
 | This program is distributed in the hope that it will be useful,         |
 | but WITHOUT ANY WARRANTY; without even the implied warranty of          |
 | MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           |
 | GNU Lesser General Public License for more details.                     |
 +-------------------------------------------------------------------------+
*/

#include "systemd_notify.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-daemon.h>
#include <systemd/sd-journal.h>
#endif

void spine_sd_ready(void) {
#ifdef HAVE_LIBSYSTEMD
    /* Two-field notify: READY plus a non-empty STATUS so `systemctl status`
     * shows something useful immediately after start-up. */
    sd_notify(0,
              "READY=1\n"
              "STATUS=Polling started\n");
#endif
}

/* Copy src into dst, replacing bytes that terminate an sd_notify field
 * (\n, \r) or that confuse journal consumers (control chars <0x20 except
 * \t, \x7f DEL, and raw \0 mid-string) with '?'. Cap at dstsz-1 bytes so
 * the output is always NUL-terminated. Shared by spine_sd_stopping and
 * spine_sd_status; NOT used for log MESSAGE sanitisation (that path has
 * a separate sanitiser in util.c so it can preserve structured keys). */
static void sd_field_sanitize(const char *src, char *dst, size_t dstsz) {
    if (dst == NULL || dstsz == 0) return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = 0;
    for (size_t i = 0; src[i] != '\0' && n + 1 < dstsz; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\n' || c == '\r' || c == '\0') {
            dst[n++] = '?';
        } else if (c < 0x20 && c != '\t') {
            dst[n++] = '?';
        } else if (c == 0x7f) {
            dst[n++] = '?';
        } else {
            dst[n++] = (char)c;
        }
    }
    dst[n] = '\0';
}

void spine_sd_stopping(const char *reason) {
#ifdef HAVE_LIBSYSTEMD
    /* \n in `reason` would inject a new sd_notify field (e.g. a forged
     * READY=1 or WATCHDOG=1) and desynchronise systemd's view of the
     * unit state. Callers have passed in user-derived strings (shutdown
     * reason codes mapped from signals, error conditions) so strip any
     * CR/LF before handing to sd_notifyf. */
    char sanitized[256];
    sd_field_sanitize(reason ? reason : "Shutting down",
                      sanitized, sizeof(sanitized));
    sd_notifyf(0,
               "STOPPING=1\n"
               "STATUS=%s\n",
               sanitized);
#else
    (void)reason;
#endif
}

void spine_sd_watchdog(void) {
#ifdef HAVE_LIBSYSTEMD
    /* sd_notify() short-circuits when NOTIFY_SOCKET is unset, so this is
     * cheap even when spine runs outside systemd. */
    sd_notify(0, "WATCHDOG=1");
#endif
}

/* Buffer the STATUS= string into a 512-byte stack array. systemd caps
 * each notification field well above that, but 512 bytes is more than
 * enough for spine's summaries (poller phase, error count, timing) and
 * keeps the TU from touching the heap on a hot path. Longer formats
 * silently truncate at 511 chars per vsnprintf's contract. */
void spine_sd_status(const char *fmt, ...) {
#ifdef HAVE_LIBSYSTEMD
    if (fmt == NULL) {
        return;  /* NULL status is a no-op; vsnprintf(NULL) is UB. */
    }
    char buf[512];
    char sanitized[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    /* Formatter may have interpolated a worker-supplied substring
     * (%s of a device hostname, SQL snippet, etc.) that contains \n.
     * Strip before sending so the field break does not leak. */
    sd_field_sanitize(buf, sanitized, sizeof(sanitized));
    sd_notifyf(0, "STATUS=%s", sanitized);
#else
    (void)fmt;
#endif
}

void spine_sd_reloading(void) {
#ifdef HAVE_LIBSYSTEMD
    /* systemd wants MONOTONIC_USEC so it can compute reload duration.
     * If clock_gettime fails (vDSO issues, sandbox), send RELOADING=1 without
     * the timestamp; systemd handles that gracefully (uses time of receipt).
     * Silently defaulting to 0 would be interpreted as a pre-boot timestamp. */
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        uint64_t monotonic_us = (uint64_t)ts.tv_sec * 1000000ULL
                              + (uint64_t)ts.tv_nsec / 1000ULL;
        sd_notifyf(0,
                   "RELOADING=1\n"
                   "MONOTONIC_USEC=%" PRIu64 "\n",
                   monotonic_us);
    } else {
        /* Intentional fprintf: this TU stays decoupled from spine.h so it
         * works before set.log_level is initialized. Callers run on the main
         * thread (SIGHUP is handled via signalfd/self-pipe, not from a raw
         * signal handler), so stdio buffering is safe. Under Type=notify,
         * systemd captures stderr into the journal, so this still reaches
         * operators without the SPINE_LOG plumbing. */
        int saved_errno = errno;
        sd_notify(0, "RELOADING=1\n");
        fprintf(stderr,
                "WARNING: clock_gettime(CLOCK_MONOTONIC) failed: %s; "
                "sd_notify reload sent without timestamp\n",
                strerror(saved_errno));
    }
#endif
}

void spine_sd_journal_log(const char *level, const char *message, int poller_id,
	unsigned long pid, unsigned long tid) {
#ifdef HAVE_LIBSYSTEMD
	int priority = LOG_INFO;

	if (level == NULL || message == NULL || !spine_sd_under_systemd()) {
		return;
	}

	if (strcmp(level, "FATAL") == 0 || strcmp(level, "ERROR") == 0) {
		priority = LOG_ERR;
	} else if (strcmp(level, "WARN") == 0 || strcmp(level, "WARNING") == 0) {
		priority = LOG_WARNING;
	} else if (strcmp(level, "DEBUG") == 0) {
		priority = LOG_DEBUG;
	}

	/* An SNMP device name, community string, or SQL fragment that made
	 * it into the log format can carry \n or ANSI control bytes. journald
	 * treats a raw \n inside MESSAGE as a field terminator on the wire
	 * protocol, which allows a crafted device name to forge synthetic
	 * journal fields (PRIORITY, SYSLOG_IDENTIFIER) downstream. 8 KiB is
	 * the sd_journal_send() hard cap; anything longer would be dropped
	 * anyway. Truncation is preferable to stack growth on the log path. */
	char safe_msg[8192];
	sd_field_sanitize(message, safe_msg, sizeof(safe_msg));

	(void) sd_journal_send(
		"MESSAGE=%s", safe_msg,
		"PRIORITY=%d", priority,
		"SYSLOG_IDENTIFIER=spine",
		"SPINE_LEVEL=%s", level,
		"SPINE_POLLER_ID=%d", poller_id,
		"SPINE_PID=%lu", pid,
		"SPINE_TID=%lu", tid,
		NULL);
#else
	(void)level;
	(void)message;
	(void)poller_id;
	(void)pid;
	(void)tid;
#endif
}

int spine_sd_under_systemd(void) {
    /* Require NOTIFY_SOCKET before claiming "under systemd". INVOCATION_ID
     * alone is set by `systemd-run --user` and inherited by manual shells
     * launched from a systemd session, which would cause spine to try to
     * sd_notify() into a non-existent socket and spam warnings. A live
     * service always has NOTIFY_SOCKET set by PID 1, so the conjunction
     * is the reliable probe. */
    if (getenv("NOTIFY_SOCKET") == NULL) {
        return 0;
    }
#ifdef HAVE_LIBSYSTEMD
    if (getenv("INVOCATION_ID") != NULL) {
        return 1;
    }
    return sd_booted() > 0;
#else
    return getenv("INVOCATION_ID") != NULL;
#endif
}
