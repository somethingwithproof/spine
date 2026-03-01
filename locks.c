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

/* _Alignas(64) prevents false sharing between threads contending
 * on different locks that would otherwise share a cache line */
_Alignas(64) static pthread_mutex_t locks[N_SPINE_LOCKS];
_Alignas(64) static pthread_cond_t  lock_cond[N_SPINE_LOCKS];

static const char * const lock_names[N_SPINE_LOCKS] = {
	[LOCK_SNMP]           = "snmp",
	[LOCK_SETEUID]        = "seteuid",
	[LOCK_GHBN]           = "ghbn",
	[LOCK_POOL]           = "pool",
	[LOCK_PHP]            = "php",
	[LOCK_PHP_PROC(0)]    = "php_proc_0",
	[LOCK_PHP_PROC(1)]    = "php_proc_1",
	[LOCK_PHP_PROC(2)]    = "php_proc_2",
	[LOCK_PHP_PROC(3)]    = "php_proc_3",
	[LOCK_PHP_PROC(4)]    = "php_proc_4",
	[LOCK_PHP_PROC(5)]    = "php_proc_5",
	[LOCK_PHP_PROC(6)]    = "php_proc_6",
	[LOCK_PHP_PROC(7)]    = "php_proc_7",
	[LOCK_PHP_PROC(8)]    = "php_proc_8",
	[LOCK_PHP_PROC(9)]    = "php_proc_9",
	[LOCK_PHP_PROC(10)]   = "php_proc_10",
	[LOCK_PHP_PROC(11)]   = "php_proc_11",
	[LOCK_PHP_PROC(12)]   = "php_proc_12",
	[LOCK_PHP_PROC(13)]   = "php_proc_13",
	[LOCK_PHP_PROC(14)]   = "php_proc_14",
	[LOCK_THDET]          = "thdet",
	[LOCK_HOST_TIME]      = "host_time",
};

void init_mutexes() {
	int i;
	for (i = 0; i < N_SPINE_LOCKS; i++) {
		pthread_mutex_init(&locks[i], PTHREAD_MUTEXATTR_DEFAULT);
		pthread_cond_init(&lock_cond[i], NULL);
	}
}

const char* get_name(int lock) {
	if (lock >= 0 && lock < N_SPINE_LOCKS && lock_names[lock] != NULL) {
		return lock_names[lock];
	}
	return "Unknown lock";
}

pthread_cond_t* get_cond(int lock) {
	SPINE_LOG_DEVDBG(("LOCKS: [RET]   Returning cond for %s", get_name(lock)));
	return &lock_cond[lock];
}

pthread_mutex_t* get_lock(int lock) {
	SPINE_LOG_DEVDBG(("LOCKS: [RET]   Returning lock for %s", get_name(lock)));
	return &locks[lock];
}

void thread_mutex_lock(int mutex) {
	SPINE_LOG_DEVDBG(("LOCKS: [START] Mutex lock for %s", get_name(mutex)));
	pthread_mutex_lock(&locks[mutex]);
	SPINE_LOG_DEVDBG(("LOCKS: [END]   Mutex lock for %s", get_name(mutex)));
}

void thread_mutex_unlock(int mutex) {
	SPINE_LOG_DEVDBG(("LOCKS: [START] Mutex unlock for %s", get_name(mutex)));
	pthread_mutex_unlock(&locks[mutex]);
	SPINE_LOG_DEVDBG(("LOCKS: [END]   Mutex unlock for %s", get_name(mutex)));
}

int thread_mutex_trylock(int mutex) {
	SPINE_LOG_DEVDBG(("LOCKS: [START] Mutex try lock for %s", get_name(mutex)));
	int ret_val = pthread_mutex_trylock(&locks[mutex]);
	SPINE_LOG_DEVDBG(("LOCKS: [END]   Mutex try lock for %s, result = %d", get_name(mutex), ret_val));
	return ret_val;
}
