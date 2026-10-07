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
#include "common.h"
#include "spine.h"
#include "locks.h"

#include <stdatomic.h>
#include <semaphore.h>
#include <time.h>

/* GNU/Linux-only executable fixture. Every wrapper forwards to the real API;
 * only task-owned main-thread allocations and registered poll requests gate.
 * The external runner owns SQL setup/readback and passes the actual CLI args. */
extern int __real_main(int argc, char **argv);
extern void *__real_calloc(size_t count, size_t size);
extern void __real_free(void *pointer);
extern int __real_uv_queue_work(uv_loop_t *loop, uv_work_t *req,
	uv_work_cb work_cb, uv_after_work_cb after_cb);
extern int __real_uv_signal_start(uv_signal_t *handle, uv_signal_cb callback, int signum);
extern int __real_pthread_mutex_unlock(pthread_mutex_t *mutex);
extern poller_thread_t **details;

static pthread_t fixture_main_thread;
static atomic_int fixture_active;
static atomic_int allocation_count;
static atomic_int released_count;
static atomic_int queued_count;
static atomic_int completed_count;
static atomic_int stop_checkpoint;
static _Atomic(uv_signal_cb) original_signal_callback;
static sem_t first_completed;
static sem_t stop_delivered;
static sem_t stop_ready;
static int ordering_case;
static spine_sem_t *initialization_sem;
static void *partition_allocations[2];
static uv_work_t *registered_requests[2];
static uv_after_work_cb original_after_callbacks[2];
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;

static void fixture_require(int admitted, const char *reason) {
	if (!admitted) {
		fprintf(stderr, "native lifecycle fixture failed: %s\n", reason);
		fflush(stderr);
		_Exit(EXIT_FAILURE);
	}
}

static void await_checkpoint(sem_t *gate, const char *reason) {
	struct timespec deadline;
	fixture_require(clock_gettime(CLOCK_REALTIME, &deadline) == 0, "checkpoint clock");
	deadline.tv_sec += 20;
	int status;
	do {
		status = sem_timedwait(gate, &deadline);
	} while (status != 0 && errno == EINTR);
	fixture_require(status == 0, reason);
}

static int on_main_thread(void) {
	return atomic_load(&fixture_active) && pthread_equal(pthread_self(), fixture_main_thread);
}

void *__wrap_calloc(size_t count, size_t size) {
	const int owned_partition = on_main_thread() && count == 1 && size == sizeof(poller_thread_t);
	if (owned_partition && ordering_case && atomic_load(&allocation_count) == 1) {
		/* Hold publication of partition 2 until the real first work callback
		 * AND its original completion callback have both finished. */
		await_checkpoint(&first_completed, "first partition completion before second allocation");
		thread_mutex_lock(LOCK_THDET);
		fixture_require(details != NULL && details[0] != NULL, "stable first aggregate");
		fixture_require(details[0]->spine_host_threads == 2, "two real device partitions");
		fixture_require(details[0]->threads_complete == 1 && !details[0]->complete,
			"first completion retained without prematurely completing device");
		thread_mutex_unlock(LOCK_THDET);
	}
	void *pointer = __real_calloc(count, size);
	if (owned_partition) {
		const int index = atomic_fetch_add(&allocation_count, 1);
		fixture_require(index < 2 && pointer != NULL, "bounded owned partition allocation");
		partition_allocations[index] = pointer;
	}
	return pointer;
}

void __wrap_free(void *pointer) {
	if (on_main_thread() && pointer != NULL) {
		if (pointer == db_pool_local) {
			for (int index = 0; index < set.threads; index++) {
				fixture_require(db_pool_local[index].free, "all local pool slots released before teardown");
			}
			int permits;
			spine_sem_getvalue(&available_threads, &permits);
			fixture_require(permits == set.threads, "all polling permits restored");
			spine_sem_getvalue(&available_scripts, &permits);
			fixture_require(permits == MAX_SIMULTANEOUS_SCRIPTS, "all actual script permits restored");
		}
		if (pointer == details && !ordering_case) {
			fixture_require(details[1] == NULL,
				"undispatched second device retains a NULL aggregate slot");
		}
		for (int index = 0; index < atomic_load(&allocation_count); index++) {
			if (pointer == partition_allocations[index]) {
				atomic_fetch_add(&released_count, 1);
				partition_allocations[index] = NULL;
				break;
			}
		}
	}
	__real_free(pointer);
}

static void after_real_poll(uv_work_t *req, int status) {
	pthread_mutex_lock(&registry_lock);
	int index = -1;
	for (int candidate = 0; candidate < 2; candidate++) {
		if (registered_requests[candidate] == req) index = candidate;
	}
	fixture_require(index >= 0 && original_after_callbacks[index] != NULL,
		"every callback belongs to a registered native producer");
	uv_after_work_cb callback = original_after_callbacks[index];
	registered_requests[index] = NULL;
	original_after_callbacks[index] = NULL;
	pthread_mutex_unlock(&registry_lock);
	fixture_require(status == 0, "real work admitted");
	callback(req, status);
	const int completed = atomic_fetch_add(&completed_count, 1) + 1;
	if (ordering_case && completed == 2) {
		thread_mutex_lock(LOCK_THDET);
		fixture_require(details[0]->threads_complete == 2 && details[0]->complete,
			"second completion preserves first count and completes aggregate");
		thread_mutex_unlock(LOCK_THDET);
	}
	if (completed == 1) fixture_require(sem_post(&first_completed) == 0, "first completion checkpoint");
}

int __wrap_uv_queue_work(uv_loop_t *loop, uv_work_t *req,
	uv_work_cb work_cb, uv_after_work_cb after_cb) {
	if (!atomic_load(&fixture_active)) return __real_uv_queue_work(loop, req, work_cb, after_cb);
	pthread_mutex_lock(&registry_lock);
	const int index = atomic_fetch_add(&queued_count, 1);
	fixture_require(index < 2 && after_cb != NULL, "bounded real poll producer registration");
	registered_requests[index] = req;
	original_after_callbacks[index] = after_cb;
	const int status = __real_uv_queue_work(loop, req, work_cb, after_real_poll);
	fixture_require(status == 0, "actual libuv work submission");
	pthread_mutex_unlock(&registry_lock);
	return status;
}

static void after_real_signal(uv_signal_t *handle, int signum) {
	uv_signal_cb callback = atomic_load(&original_signal_callback);
	fixture_require(callback != NULL, "registered actual signal callback");
	callback(handle, signum);
	if (signum == SIGTERM) fixture_require(sem_post(&stop_delivered) == 0, "real stop signal acknowledgement");
}

int __wrap_uv_signal_start(uv_signal_t *handle, uv_signal_cb callback, int signum) {
	if (!atomic_load(&fixture_active)) return __real_uv_signal_start(handle, callback, signum);
	uv_signal_cb previous = atomic_load(&original_signal_callback);
	fixture_require(previous == NULL || previous == callback, "one actual production signal handler");
	atomic_store(&original_signal_callback, callback);
	int status = __real_uv_signal_start(handle, after_real_signal, signum);
	if (status == 0 && signum == SIGTERM) {
		fixture_require(sem_post(&stop_ready) == 0, "registered stop signal checkpoint");
	}
	return status;
}

int __wrap_pthread_mutex_unlock(pthread_mutex_t *mutex) {
	const int status = __real_pthread_mutex_unlock(mutex);
	if (status == 0 && on_main_thread() && !ordering_case &&
		atomic_load(&allocation_count) == 1) {
		/* Main owns a local initialization semaphore. Capture its borrowed
		 * address from the published immutable aggregate exactly once;
		 * never revisit details after main has freed that array. */
		if (initialization_sem == NULL && details != NULL && details[0] != NULL) {
			initialization_sem = details[0]->thread_init_sem;
		}
		if (initialization_sem != NULL && mutex == &initialization_sem->mutex &&
			initialization_sem->value == 1 && !atomic_exchange(&stop_checkpoint, 1)) {
			/* Main has accepted and posted its first initialization permit.
			 * Its next iteration must observe SIGTERM before device 2. */
			await_checkpoint(&stop_ready, "actual stop signal registration before delivery");
			fixture_require(kill(getpid(), SIGTERM) == 0, "deliver owned-process SIGTERM");
			await_checkpoint(&stop_delivered, "actual production stop handler");
		}
	}
	return status;
}

static void verify_native_exit(void) {
	const int expected = ordering_case ? 2 : 1;
	fixture_require(set.exit_code == EXIT_SUCCESS, "native graceful completion outcome");
	fixture_require(atomic_load(&allocation_count) == expected &&
		atomic_load(&queued_count) == expected && atomic_load(&completed_count) == expected,
		"all and only expected partitions admitted and completed");
	fixture_require(atomic_load(&released_count) == expected, "every owned partition freed exactly once");
	if (!ordering_case) fixture_require(atomic_load(&stop_checkpoint) == 1, "early-stop checkpoint executed");
	for (int index = 0; index < 2; index++) {
		fixture_require(registered_requests[index] == NULL && original_after_callbacks[index] == NULL,
			"no dangling callback producer registration");
	}
	fixture_require(sem_destroy(&first_completed) == 0 && sem_destroy(&stop_delivered) == 0 && sem_destroy(&stop_ready) == 0,
		"owned checkpoint cleanup");
	fprintf(stderr, "NATIVE_LIFECYCLE_%s_PASS\n", ordering_case ? "PARTITION_ORDER" : "EARLY_STOP");
}

int __wrap_main(int argc, char **argv) {
	if (argc == 3 && strcmp(argv[1], "--fixture-producer") == 0) {
		fixture_require(strcmp(argv[2], "41") == 0 || strcmp(argv[2], "42") == 0 ||
			strcmp(argv[2], "51") == 0 || strcmp(argv[2], "52") == 0, "owned producer value");
		return printf("%s\n", argv[2]) > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
	}
	const char *scenario = getenv("SPINE_NATIVE_LIFECYCLE_SCENARIO");
	fixture_require(scenario != NULL && (strcmp(scenario, "partition-order") == 0 ||
		strcmp(scenario, "early-stop") == 0), "explicit native fixture scenario");
	ordering_case = strcmp(scenario, "partition-order") == 0;
	fixture_main_thread = pthread_self();
	fixture_require(sem_init(&first_completed, 0, 0) == 0 && sem_init(&stop_delivered, 0, 0) == 0 && sem_init(&stop_ready, 0, 0) == 0,
		"owned checkpoint initialization");
	fixture_require(atexit(verify_native_exit) == 0, "native outcome registration");
	atomic_store(&fixture_active, 1);
	return __real_main(argc, argv);
}
