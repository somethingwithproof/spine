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

/* Test-only interposition around the real libuv submission calls. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

static int selected(const char *mode) {
	const char *requested = getenv("SPINE_TEST_SUBMISSION_FAULT");
	return requested != NULL && strcmp(requested, mode) == 0;
}

int uv_queue_work(uv_loop_t *loop, uv_work_t *request, uv_work_cb work, uv_after_work_cb after) {
	if (selected("queue")) {
		fputs("FIXTURE: rejected uv_queue_work\n", stderr);
		return UV_ENOMEM;
	}
	int (*original)(uv_loop_t *, uv_work_t *, uv_work_cb, uv_after_work_cb) = dlsym(RTLD_NEXT, "uv_queue_work");
	if (original == NULL) abort();
	return original(loop, request, work, after);
}

int uv_async_send(uv_async_t *handle) {
	static atomic_int sends;
	if (selected("wake") && atomic_fetch_add(&sends, 1) == 0) {
		fputs("FIXTURE: rejected first uv_async_send\n", stderr);
		return UV_EINVAL;
	}
	int (*original)(uv_async_t *) = dlsym(RTLD_NEXT, "uv_async_send");
	if (original == NULL) abort();
	return original(handle);
}
