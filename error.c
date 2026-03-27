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

/* These functions handle simple signal handling functions for Spine.  It was
   written to handle specifically issues with the Solaris threading model in
   version 2.8.
*/

#include "common.h"
#include "spine.h"

/*! \fn static void spine_signal_handler(int spine_signal)
 *  \brief interrupts the os default signal handler as appropriate.
 *
 */
static void spine_signal_handler(int spine_signal) {
	signal(spine_signal, SIG_DFL);
	set.exit_code = spine_signal;

	const char *msg = "FATAL: Spine Interrupted by signal\n";
	int dummy = write(STDERR_FILENO, msg, strlen(msg));
	(void)dummy;

	_exit(spine_signal);
}

static int spine_fatal_signals[] = {
	SIGINT,
	SIGPIPE,
	SIGSEGV,
	SIGBUS,
	SIGFPE,
	SIGQUIT,
	SIGSYS,
	SIGABRT,
	0
};

/*! \fn void install_spine_signal_handler(void)
 *  \brief installs the spine signal handler to stop certain calls from
 *         abending Spine.
 *
 */
void install_spine_signal_handler(void) {
	/* Set a handler for any fatal signal not already handled */
	int i;
	struct sigaction sa;
	void (*ohandler)(int);

	for (i=0; spine_fatal_signals[i]; ++i) {
		sigaction(spine_fatal_signals[i], NULL, &sa);
		if (sa.sa_handler == SIG_DFL) {
			sa.sa_handler = spine_signal_handler;
			sigemptyset(&sa.sa_mask);
			sa.sa_flags = SA_RESTART;
			sigaction(spine_fatal_signals[i], &sa, NULL);
		}
	}

	for (i=0; spine_fatal_signals[i]; ++i) {
		ohandler = signal(spine_fatal_signals[i], spine_signal_handler);
		if (ohandler != SIG_DFL) {
			signal(spine_fatal_signals[i], ohandler);
		}
	}

	return;
}

/*! \fn void uninstall_spine_signal_handler(void)
 *  \brief uninstalls the spine signal handler.
 *
 */
void uninstall_spine_signal_handler(void) {
	/* Remove a handler for any fatal signal handled */
	int i;
	struct sigaction sa;
	void (*ohandler)(int);

	for (i=0; spine_fatal_signals[i]; ++i) {
		sigaction(spine_fatal_signals[i], NULL, &sa);
		if (sa.sa_handler == spine_signal_handler) {
			sa.sa_handler = SIG_DFL;
			sigaction(spine_fatal_signals[i], &sa, NULL);
		}
	}

	for ( i=0; spine_fatal_signals[i]; ++i ) {
		ohandler = signal(spine_fatal_signals[i], SIG_DFL);
		if (ohandler != spine_signal_handler) {
			signal(spine_fatal_signals[i], ohandler);
		}
	}
}
