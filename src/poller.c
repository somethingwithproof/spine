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
#include "spine_probes.h"
#include "circuit_breaker.h"
#include "host_polling_service.h"
#include "host_polling_stages.h"
#include "poll_state.h"
#include "output_buffer.h"
#include "platform/platform_fd.h"

#ifdef HAVE_LIBUV
#include "poll_state_internal.h"
#include "async_dns.h"
#include "async_snmp.h"
#include "async_exec.h"
#include "async_php.h"
#include "async_mysql.h"
#include "async_batch.h"
#include "task_scheduler.h"
#include "task_executor.h"
#endif

static int poll_host_run(int device_counter, int host_id, int spine_host_thread, int spine_host_threads, int host_data_ids, char *spine_host_time, int *host_errors, double spine_host_time_double);

static void append_output_query(char *buffer, size_t capacity, const char *part, size_t length) {
	if (!spine_output_buffer_append(buffer, capacity, part, length)) {
		set.exit_code = EXIT_FAILURE;
		die("ERROR: Poller output query exceeds its allocated buffer");
	}
}

#ifdef HAVE_LIBUV
typedef struct {
	uv_work_t work;
	poller_thread_t *details;
	int poll_status;
} spine_work_t;

static void spine_poll_work_cb(uv_work_t *req) {
	spine_work_t *sw = (spine_work_t *)req->data;
	poller_thread_t *det = sw->details;
	int host_errors = 0;

	spine_platform_set_thread_name("spine-poll");

	if (spine_cb_should_skip(det->host_id)) {
		SPINE_LOG_MEDIUM(("Device[%i] skipped by circuit breaker", det->host_id));
	} else {
		sw->poll_status = poll_host_run(det->device_counter, det->host_id, det->spine_host_thread, det->spine_host_threads, det->host_data_ids, det->spine_host_time, &host_errors, det->spine_host_time_double);
		spine_cb_record(det->host_id, host_errors);
	}
}

static void spine_after_poll_work_cb(uv_work_t *req, int status) {
	spine_work_t *sw = (spine_work_t *)req->data;
	poller_thread_t *det = sw->details;

	det->complete = (status == 0 && sw->poll_status == 0);
	if (!det->complete) {
		spine_poll_work_failed();
	}
	SPINE_LOG_DEVDBG(("DEBUG: Device[%i] HT[%i] Poll work complete (status=%d)", det->host_id, det->spine_host_thread, status));

	/* Post to available_threads to signal slot availability */
	spine_sem_post(&available_threads);

	free(sw);
}

int spine_queue_poll(poller_thread_t *det) {
	spine_work_t *sw = malloc(sizeof(spine_work_t));
	int rc;
	if (sw == NULL) {
		return ENOMEM;
	}
	sw->work.data = sw;
	sw->details = det;
	sw->poll_status = 0;

	rc = uv_queue_work(det->event_loop, &sw->work, spine_poll_work_cb, spine_after_poll_work_cb);
	if (rc != 0) {
		free(sw);
		return EAGAIN;
	}

	return 0;
}
#endif

int format_poller_output_row(char *output, size_t output_size,
		int local_data_id, const char *escaped_rrd_name,
		const char *host_time, const char *escaped_result) {
	const char *timep;
	int decimal_points = 0;
	int digits = 0;
	int written;

	if (output == NULL || output_size == 0 || escaped_rrd_name == NULL ||
	    host_time == NULL || host_time[0] == '\0' || escaped_result == NULL) {
		return FALSE;
	}

	/* host_time is emitted outside SQL quotes. Accept the integer timestamps
	 * Spine generates and a single fractional part, but no SQL syntax. */
	for (timep = host_time; *timep != '\0'; timep++) {
		if (*timep == '.') {
			decimal_points++;
			if (decimal_points > 1) {
				return FALSE;
			}
		} else if (!isdigit((unsigned char)*timep)) {
			return FALSE;
		} else {
			digits++;
		}
	}
	if (digits == 0) {
		return FALSE;
	}

	written = snprintf(output, output_size,
		" (%i, '%s', FROM_UNIXTIME(%s), '%s')",
		local_data_id, escaped_rrd_name, host_time, escaped_result);

	return written >= 0 && (size_t)written < output_size;
}

void child_cleanup(void *arg) {
	poller_thread_t poller_details = *(poller_thread_t*) arg;

	SPINE_LOG_DEVDBG(("Device[%i] HT[%i] DEBUG: The Device Thread has cleaned up.", poller_details.host_id, poller_details.spine_host_thread));

	child_cleanup_thread(arg);
}

void child_cleanup_thread(void *arg) {
	UNUSED_PARAMETER(arg);
	spine_sem_post(&available_threads);

	int a_threads_value;
	spine_sem_getvalue(&available_threads, &a_threads_value);

	SPINE_LOG_DEVDBG(("DEBUG: Available Threads is %i (%i outstanding)", a_threads_value, set.threads - a_threads_value));
}

void child_cleanup_script(void *arg) {
	UNUSED_PARAMETER(arg);
	spine_sem_post(&available_scripts);

	int a_scripts_value;
	spine_sem_getvalue(&available_scripts, &a_scripts_value);

	SPINE_LOG_DEVDBG(("DEBUG: Available Scripts is %i (%i outstanding)", a_scripts_value, MAX_SIMULTANEOUS_SCRIPTS - a_scripts_value));
}

/*! \fn void *child(void *arg)
 *  \brief function is called via the fork command and initiates a poll of a host
 *  \param arg a pointer to an integer point to the host_id to be polled
 *
 *	This function will call the primary Spine polling function to poll a host
 *  and then reduce the number of active threads by one so that the next host
 *  can be polled.
 *
 */
void *child(void *arg) {
	pthread_cleanup_push(child_cleanup, arg);

	int device_counter;
	int host_id;
	int spine_host_thread;
	int spine_host_threads;
	int host_data_ids;
	int host_errors;
	double spine_host_time_double;
	char spine_host_time[SMALL_BUFSIZE];

	/* Name the thread before any real work so that ps -L, top -H, or
	 * perf report show each poll worker distinctly. Linux truncates at
	 * 15 bytes + NUL, so the "spine-poll" prefix leaves room for a 4-digit
	 * host id in the 15-byte budget. */
	spine_platform_set_thread_name("spine-poll");

	host_errors = 0;

	poller_thread_t poller_details = *(poller_thread_t*) arg;

	device_counter   = poller_details.device_counter;
	host_id          = poller_details.host_id;
	spine_host_thread      = poller_details.spine_host_thread;
	spine_host_threads     = poller_details.spine_host_threads;
	host_data_ids    = poller_details.host_data_ids;
	spine_host_time_double = poller_details.spine_host_time_double;

	snprintf(spine_host_time, SMALL_BUFSIZE, "%s", poller_details.spine_host_time);

	thread_mutex_unlock(LOCK_HOST_TIME);

	/* Allows main thread to proceed with creation of other threads */
	spine_sem_post(poller_details.thread_init_sem);

	if (is_debug_device(host_id)) {
		SPINE_LOG(("Device[%i] HT[%i] DEBUG: In Poller, About to Start Polling", host_id, spine_host_thread));
	} else {
		SPINE_LOG_DEBUG(("Device[%i] HT[%i] DEBUG: In Poller, About to Start Polling", host_id, spine_host_thread));
	}

	if (spine_cb_should_skip(host_id)) {
		SPINE_LOG_MEDIUM(("Device[%i] skipped by circuit breaker", host_id));
	} else {
		poll_host(device_counter, host_id, spine_host_thread, spine_host_threads, host_data_ids, spine_host_time, &host_errors, spine_host_time_double);
		spine_cb_record(host_id, host_errors);
	}

	pthread_cleanup_pop(1);

	/* end the thread */
	pthread_exit(0);

	exit(0);
}

/*! \fn int poller_store_hex_result(char *result, size_t result_size, const char *hex, int *errors)
 *  \brief convert a hexadecimal poll result and account for rejected values
 *
 *  result must name at least two writable bytes so failures can be represented
 *  by the normal undefined marker. Invalid output arguments are rejected
 *  without modifying memory whose writable extent cannot be established.
 */
int poller_store_hex_result(char *result, size_t result_size, const char *hex, int *errors) {
	unsigned long long value;
	int written;

	if (result == NULL || result_size < 2 || hex == NULL) {
		if (errors != NULL) {
			(*errors)++;
		}
		return FALSE;
	}

	if (!hex2dec(hex, &value)) {
		/* An over-wide or malformed OctetString is unusable poll data. Count it
		 * like every other undefined result so host_errors reflects the failed
		 * collection; callers also retain the affected local_data_id. */
		SET_UNDEFINED(result);
		if (errors != NULL) {
			(*errors)++;
		}
		return FALSE;
	}

	written = snprintf(result, result_size, "%llu", value);
	if (written < 0 || (size_t) written >= result_size) {
		SET_UNDEFINED(result);
		if (errors != NULL) {
			(*errors)++;
		}
		return FALSE;
	}

	return TRUE;
}

typedef struct HostPollPipelineData HostPollPipelineData;

static ResultCode host_poll_stage_poll_items(const HostPollingRequest *request, HostPollingStageOutput *output);
static ResultCode host_poll_stage_persist_results(const HostPollingRequest *request, HostPollingStageOutput *output);
static ResultCode host_poll_stage_update_host_state(const HostPollingRequest *request, HostPollingStageOutput *output);
static ResultCode host_poll_executor_legacy(const HostPollingRequest *request, HostPollingStageOutput *output);
static void poll_host_legacy(int host_id, int spine_host_thread,
	int host_data_ids, char *spine_host_time,
	int *host_errors,
	HostPollPipelineData *pipeline_data);

typedef struct HostPollPipelineData {
	int host_errors;
	char error_data_ids[DBL_BUFSIZE];
	target_t *poller_items;
	int rows_processed;
	char query8[BUFSIZE];
	char query11[BUFSIZE];
	char posuffix[BUFSIZE];
	int query8_len;
	int query11_len;
	int posuffix_len;
} HostPollPipelineData;

static int poll_host_run(int device_counter, int host_id, int spine_host_thread, int spine_host_threads, int host_data_ids, char *spine_host_time, int *host_errors, double spine_host_time_double) {
	HostPollingRequest request;
	HostPollingResult result;
	HostPollPipelineData pipeline_data;

	memset(&request, 0, sizeof(request));
	memset(&pipeline_data, 0, sizeof(pipeline_data));
	request.device_counter = device_counter;
	request.host_id = host_id;
	request.spine_host_thread = spine_host_thread;
	request.spine_host_threads = spine_host_threads;
	request.host_data_ids = host_data_ids;
	request.spine_host_time = spine_host_time;
	request.host_errors = host_errors;
	request.spine_host_time_double = spine_host_time_double;
	request.user_data = &pipeline_data;
	request.max_retries = 1;
	request.on_load_work_items = host_polling_stage_load_work_items;
	request.on_check_availability = host_polling_stage_check_availability;
	request.on_poll_items = host_poll_stage_poll_items;
	request.on_persist_results = host_poll_stage_persist_results;
	request.on_update_host_state = host_poll_stage_update_host_state;

	result = host_polling_service_run(&request);
	if (host_errors != NULL) {
		*host_errors = result.host_errors;
	}

	if (result.code != RESULT_CODE_OK) {
		SPINE_LOG(("ERROR: Device[%d] pipeline failed at stage %d after %d retries",
			host_id, (int)result.failed_stage, result.retries_used));
	}
	return result.code == RESULT_CODE_OK ? 0 : EIO;
}

void poll_host(int device_counter, int host_id, int spine_host_thread, int spine_host_threads, int host_data_ids, char *spine_host_time, int *host_errors, double spine_host_time_double) {
	(void)poll_host_run(device_counter, host_id, spine_host_thread, spine_host_threads, host_data_ids, spine_host_time, host_errors, spine_host_time_double);
}

static ResultCode host_poll_stage_poll_items(const HostPollingRequest *request, HostPollingStageOutput *output) {
	return host_poll_executor_legacy(request, output);
}

static ResultCode host_poll_executor_legacy(const HostPollingRequest *request, HostPollingStageOutput *output) {
	int host_errors = 0;
	HostPollPipelineData *pipeline_data = (HostPollPipelineData *)request->user_data;

	poll_host_legacy(request->host_id, request->spine_host_thread,
		request->host_data_ids, request->spine_host_time,
		&host_errors, pipeline_data);

	output->host_errors = host_errors;
	output->retryable = 0;
	if (pipeline_data != NULL) {
		pipeline_data->host_errors = host_errors;
	}

	return RESULT_CODE_OK;
}

typedef struct {
	MYSQL *mysql;
	int mode;
	char *normal;
	char *boost;
	size_t capacity;
	size_t used;
	int new_buffer;
} persisted_output_t;

static void release_persist_connections(const pool_t *local_cnn, const pool_t *remote_cnn) {
	if (remote_cnn != NULL) {
		db_release_connection(REMOTE, remote_cnn->id);
	}
	db_release_connection(LOCAL, local_cnn->id);
}

/* Normal output precedes Boost on both batch and final writes. Reset only
 * batch writes: the final buffers remain owned until the caller frees them. */
static void flush_persisted_output(persisted_output_t *writer, const HostPollPipelineData *data, int reset) {
	append_output_query(writer->normal, writer->capacity, data->posuffix, (size_t)data->posuffix_len);
	db_insert(writer->mysql, writer->mode, writer->normal);
	if (reset) {
		memset(writer->normal, 0, writer->capacity);
		append_output_query(writer->normal, writer->capacity, data->query8, (size_t)data->query8_len);
	}
	if (set.boost_redirect && set.boost_enabled) {
		append_output_query(writer->boost, writer->capacity, data->posuffix, (size_t)data->posuffix_len);
		db_insert(writer->mysql, writer->mode, writer->boost);
		if (reset) {
			memset(writer->boost, 0, writer->capacity);
			append_output_query(writer->boost, writer->capacity, data->query11, (size_t)data->query11_len);
		}
	}
}

static void append_persisted_row(persisted_output_t *writer, const HostPollPipelineData *data,
	const HostPollingRequest *request, int i) {
	char result_string[(RESULTS_BUFFER * 2) + DBL_BUFSIZE + SMALL_BUFSIZE];
	/* Escaping can double each source byte, plus the terminator. */
	char escaped_result[(RESULTS_BUFFER * 2) + 1];
	char escaped_rrd_name[DBL_BUFSIZE];

	db_escape(writer->mysql, escaped_result, sizeof(escaped_result), data->poller_items[i].result);
	db_escape(writer->mysql, escaped_rrd_name, sizeof(escaped_rrd_name), data->poller_items[i].rrd_name);

	if (!format_poller_output_row(result_string, sizeof(result_string),
		data->poller_items[i].local_data_id,
		escaped_rrd_name,
		request->spine_host_time,
		escaped_result)) {
		SPINE_LOG(("Device[%i] HT[%i] ERROR: Poller output for DS[%i] "
			"exceeds the configured result buffer and was skipped",
			request->host_id, request->spine_host_thread,
			data->poller_items[i].local_data_id));
		return;
	}
	const int result_length = (int)strlen(result_string);

	if (spine_output_buffer_needs_flush(writer->used, (size_t)result_length, MAX_MYSQL_BUF_SIZE)) {
		flush_persisted_output(writer, data, TRUE);
		writer->used = strlen(writer->normal);
		writer->new_buffer = TRUE;
	}

	result_string[0] = writer->new_buffer ? ' ' : ',';
	append_output_query(writer->normal, writer->capacity, result_string, (size_t)result_length);
	if (set.boost_redirect && set.boost_enabled) {
		append_output_query(writer->boost, writer->capacity, result_string, (size_t)result_length);
	}
	writer->used += strlen(result_string);
	writer->new_buffer = FALSE;
}

static ResultCode persist_output_rows(const HostPollingRequest *request, HostPollPipelineData *data,
	MYSQL *mysql, int mode) {
	if (data->poller_items == NULL || data->rows_processed <= 0) {
		return RESULT_CODE_OK;
	}
	persisted_output_t writer = { .mysql = mysql, .mode = mode, .new_buffer = TRUE };
	writer.capacity = spine_output_buffer_size(MAX_MYSQL_BUF_SIZE,
		sizeof(char[(RESULTS_BUFFER * 2) + DBL_BUFSIZE + SMALL_BUFSIZE]),
		(size_t)(data->query8_len > data->query11_len ? data->query8_len : data->query11_len),
		(size_t)data->posuffix_len);
	if (writer.capacity == 0) {
		set.exit_code = EXIT_FAILURE;
		die("ERROR: Poller output query buffer size overflow");
	}
	writer.normal = malloc(writer.capacity);
	if (writer.normal == NULL) {
		SPINE_FREE(data->poller_items);
		return RESULT_CODE_ERROR;
	}
	memset(writer.normal, 0, writer.capacity);
	append_output_query(writer.normal, writer.capacity, data->query8, (size_t)data->query8_len);
	writer.used = strlen(writer.normal);
	if (set.boost_redirect && set.boost_enabled) {
		writer.boost = malloc(writer.capacity);
		if (writer.boost == NULL) {
			SPINE_FREE(data->poller_items);
			free(writer.normal);
			return RESULT_CODE_ERROR;
		}
		memset(writer.boost, 0, writer.capacity);
		append_output_query(writer.boost, writer.capacity, data->query11, (size_t)data->query11_len);
	}
	for (int i = 0; i < data->rows_processed; i++) {
		append_persisted_row(&writer, data, request, i);
	}
	if (writer.used > strlen(data->query8)) {
		flush_persisted_output(&writer, data, FALSE);
	}
	free(writer.normal);
	if (writer.boost != NULL) {
		free(writer.boost);
	}
	SPINE_FREE(data->poller_items);
	return RESULT_CODE_OK;
}

static void advance_persisted_schedule(const HostPollingRequest *request, MYSQL *mysql) {
	if (request->spine_host_thread == request->spine_host_threads && set.active_profiles != 1) {
		char poller_next_step_query[BUFSIZE];
		SPINE_LOG_MEDIUM(("Device[%i] HT[%i] Updating Poller Items for Next Poll",
			request->host_id, request->spine_host_thread));
		if (set.poller_id == 0) {
			snprintf(poller_next_step_query, sizeof(poller_next_step_query),
				"UPDATE poller_item"
				" SET rrd_next_step = IF(rrd_step = %i, 0, IF(rrd_next_step - %i < 0, rrd_step - %i, rrd_next_step - %i))"
				" WHERE host_id = %i",
				set.poller_interval, set.poller_interval, set.poller_interval, set.poller_interval, request->host_id);
		} else {
			snprintf(poller_next_step_query, sizeof(poller_next_step_query),
				"UPDATE poller_item"
				" SET rrd_next_step = IF(rrd_step = %i, 0, IF(rrd_next_step - %i < 0, rrd_step - %i, rrd_next_step - %i))"
				" WHERE host_id = %i"
				" AND poller_id = %i",
				set.poller_interval, set.poller_interval, set.poller_interval, set.poller_interval, request->host_id, set.poller_id);
			}
			db_query(mysql, LOCAL, poller_next_step_query);
	}

}

static ResultCode host_poll_stage_persist_results(const HostPollingRequest *request, HostPollingStageOutput *output) {
	HostPollPipelineData *pipeline_data = (HostPollPipelineData *)request->user_data;
	pool_t *local_cnn;
	pool_t *remote_cnn = NULL;
	MYSQL mysql;
	MYSQL mysqlr;
	MYSQL mysqlt;
	int mode;

	if (pipeline_data == NULL) {
		output->host_errors = request->host_errors ? *request->host_errors : 0;
		output->retryable = 0;
		return RESULT_CODE_OK;
	}

	local_cnn = db_get_connection(LOCAL);
	if (local_cnn == NULL) {
		SPINE_FREE(pipeline_data->poller_items);
		output->retryable = 1;
		return RESULT_CODE_ERROR;
	}
	mysql = local_cnn->mysql;
	if (set.poller_id > 1 && set.mode == REMOTE_ONLINE) {
		remote_cnn = db_get_connection(REMOTE);
		if (remote_cnn == NULL) {
			SPINE_FREE(pipeline_data->poller_items);
			db_release_connection(LOCAL, local_cnn->id);
			output->retryable = 1;
			return RESULT_CODE_ERROR;
		}
		mysqlr = remote_cnn->mysql;
		mysqlt = mysqlr;
		mode = REMOTE;
	} else {
		mysqlt = mysql;
		mode = LOCAL;
	}

	if (persist_output_rows(request, pipeline_data, &mysqlt, mode) != RESULT_CODE_OK) {
		release_persist_connections(local_cnn, remote_cnn);
		output->retryable = 0;
		return RESULT_CODE_ERROR;
	}

	advance_persisted_schedule(request, &mysql);

	if (pipeline_data->host_errors > 0) {
		int error_query_len = (int)strlen(pipeline_data->error_data_ids) + BUFSIZE;
		char *error_query = malloc((size_t)error_query_len);
		if (error_query == NULL) {
			SPINE_FREE(pipeline_data->poller_items);
			if (remote_cnn != NULL) {
				db_release_connection(REMOTE, remote_cnn->id);
			}
			db_release_connection(LOCAL, local_cnn->id);
			output->retryable = 0;
			return RESULT_CODE_ERROR;
		}

		snprintf(error_query, (size_t)error_query_len, "INSERT INTO host_errors (host_id, poller_id, errors, local_data_ids)"
			" VALUES(%i, %i, %i, '%s')"
			" ON DUPLICATE KEY UPDATE"
			" errors = errors + VALUES(errors),"
			" local_data_ids = CONCAT(local_data_ids, ', ', VALUES(local_data_ids))",
			request->host_id, set.poller_id, pipeline_data->host_errors, pipeline_data->error_data_ids);

		db_query(&mysql, LOCAL, error_query);
		free(error_query);
	}

	release_persist_connections(local_cnn, remote_cnn);

	output->host_errors = request->host_errors ? *request->host_errors : 0;
	output->retryable = 0;
	return RESULT_CODE_OK;
}

static ResultCode host_poll_stage_update_host_state(const HostPollingRequest *request, HostPollingStageOutput *output) {
	extern poller_thread_t** details;
	const pool_t *local_cnn;
	MYSQL mysql;

	local_cnn = db_get_connection(LOCAL);
	if (local_cnn == NULL) {
		output->retryable = 1;
		return RESULT_CODE_ERROR;
	}
	mysql = local_cnn->mysql;

	thread_mutex_lock(LOCK_THDET);
	details[request->device_counter]->threads_complete++;
	if (details[request->device_counter]->threads_complete == details[request->device_counter]->spine_host_threads) {
		details[request->device_counter]->complete = TRUE;
		char query[BUFSIZE];
		double poll_time = get_time_as_double();
		snprintf(query, sizeof(query), "UPDATE host SET polling_time = %.3f - %.3f WHERE id = %i",
			poll_time, request->spine_host_time_double, request->host_id);
		db_query(&mysql, LOCAL, query);
	}
	thread_mutex_unlock(LOCK_THDET);

	db_release_connection(LOCAL, local_cnn->id);
	output->host_errors = request->host_errors ? *request->host_errors : 0;
	output->retryable = 0;
	return RESULT_CODE_OK;
}


/* Own the SQL buffers for one host partition. Array capacities and query
 * construction stay identical to the legacy caller. */
typedef struct {
	char query1[BUFSIZE];
	char query2[BIG_BUFSIZE];
	char query4[BUFSIZE];
	char query5[BUFSIZE];
	char query8[BUFSIZE];
	char query9[BUFSIZE];
	char query10[BUFSIZE];
	char query11[BUFSIZE];
	char posuffix[BUFSIZE];
	int query8_len;
	int query11_len;
	int posuffix_len;
} legacy_queries_t;

static void build_unscoped_poll_queries(legacy_queries_t *queries, int host_id, const char *limits, const char *regex_col) {
	if (set.total_snmp_ports == 1) {
		snprintf(queries->query1, BUFSIZE,
			"SELECT SQL_NO_CACHE action, hostname, snmp_community, "
				"snmp_version, snmp_username, snmp_password, "
				"rrd_name, rrd_path, arg1, arg2, arg3, local_data_id, "
				"rrd_num, snmp_port, snmp_timeout, "
				"snmp_auth_protocol, snmp_priv_passphrase, snmp_priv_protocol, snmp_context, snmp_engine_id"
				"%s"
			" FROM poller_item"
			" WHERE host_id = %i"
			" AND deleted = '' %s", regex_col, host_id, limits);
	} else {
		snprintf(queries->query1, BUFSIZE,
			"SELECT SQL_NO_CACHE action, hostname, snmp_community, "
				"snmp_version, snmp_username, snmp_password, "
				"rrd_name, rrd_path, arg1, arg2, arg3, local_data_id, "
				"rrd_num, snmp_port, snmp_timeout, "
				"snmp_auth_protocol, snmp_priv_passphrase, snmp_priv_protocol, snmp_context, snmp_engine_id"
				"%s"
			" FROM poller_item"
			" WHERE host_id = %i"
			" AND deleted = ''"
			" ORDER BY snmp_port %s", regex_col, host_id, limits);
	}

	/* host structure for uptime checks */
	snprintf(queries->query2, BIG_BUFSIZE,
		"SELECT SQL_NO_CACHE id, hostname, snmp_community, snmp_version, "
			"snmp_username, snmp_password, snmp_auth_protocol, "
			"snmp_priv_passphrase, snmp_priv_protocol, snmp_context, snmp_engine_id, snmp_port, snmp_timeout, max_oids, "
			"availability_method, ping_method, ping_port, ping_timeout, ping_retries, "
			"status, status_event_count, UNIX_TIMESTAMP(status_fail_date), "
			"UNIX_TIMESTAMP(status_rec_date), status_last_error, "
			"min_time, max_time, cur_time, avg_time, "
			"total_polls, failed_polls, availability, snmp_sysUpTimeInstance, snmp_sysDescr, snmp_sysObjectID, "
                "snmp_sysContact, snmp_sysName, snmp_sysLocation"
		" FROM host"
		" WHERE id = %i"
		" AND deleted = ''", host_id);

	/* data query structure for reindex detection */
	snprintf(queries->query4, BUFSIZE,
		"SELECT SQL_NO_CACHE data_query_id, action, op, assert_value, arg1"
			" FROM poller_reindex"
			" WHERE host_id = %i", host_id);

	/* multiple polling interval query for items */
	if (set.active_profiles != 1) {
		if (set.total_snmp_ports == 1) {
			snprintf(queries->query5, BUFSIZE,
				"SELECT SQL_NO_CACHE action, hostname, snmp_community, "
					"snmp_version, snmp_username, snmp_password, "
					"rrd_name, rrd_path, arg1, arg2, arg3, local_data_id, "
					"rrd_num, snmp_port, snmp_timeout, "
					"snmp_auth_protocol, snmp_priv_passphrase, snmp_priv_protocol, snmp_context, snmp_engine_id"
					"%s"
				" FROM poller_item"
				" WHERE host_id = %i"
				" AND rrd_next_step <= 0"
				" %s", regex_col, host_id, limits);
		} else {
			snprintf(queries->query5, BUFSIZE,
				"SELECT SQL_NO_CACHE action, hostname, snmp_community, "
					"snmp_version, snmp_username, snmp_password, "
					"rrd_name, rrd_path, arg1, arg2, arg3, local_data_id, "
					"rrd_num, snmp_port, snmp_timeout, "
					"snmp_auth_protocol, snmp_priv_passphrase, snmp_priv_protocol, snmp_context, snmp_engine_id"
					"%s"
				" FROM poller_item"
				" WHERE host_id = %i"
				" AND rrd_next_step <= 0"
				" ORDER BY snmp_port %s", regex_col, host_id, limits);
		}
	} else {
		if (set.total_snmp_ports == 1) {
			snprintf(queries->query5, BUFSIZE,
				"SELECT SQL_NO_CACHE action, hostname, snmp_community, "
					"snmp_version, snmp_username, snmp_password, "
					"rrd_name, rrd_path, arg1, arg2, arg3, local_data_id, "
					"rrd_num, snmp_port, snmp_timeout, "
					"snmp_auth_protocol, snmp_priv_passphrase, snmp_priv_protocol, snmp_context, snmp_engine_id"
					"%s"
				" FROM poller_item"
				" WHERE host_id = %i"
				" %s", regex_col, host_id, limits);
		} else {
			snprintf(queries->query5, BUFSIZE,
				"SELECT SQL_NO_CACHE action, hostname, snmp_community, "
					"snmp_version, snmp_username, snmp_password, "
					"rrd_name, rrd_path, arg1, arg2, arg3, local_data_id, "
					"rrd_num, snmp_port, snmp_timeout, "
					"snmp_auth_protocol, snmp_priv_passphrase, snmp_priv_protocol, snmp_context, snmp_engine_id"
					"%s"
				" FROM poller_item"
				" WHERE host_id = %i"
				" ORDER BY snmp_port %s", regex_col, host_id, limits);
		}
	}

	/* query to add output records to the poller output table */
	snprintf(queries->query8, BUFSIZE,
		"INSERT INTO poller_output"
		" (local_data_id, rrd_name, time, output) VALUES");

	/* set.dbonupdate describes the local connection, while results can be
	 * written to the remote one. VALUES() is accepted by both MySQL and
	 * MariaDB, so use it until the remote server has its own version
	 * capability flag (#590). */
	snprintf(queries->posuffix, BUFSIZE,
		" ON DUPLICATE KEY UPDATE output=VALUES(output)");

	/* number of agent's count for single polling interval */
	snprintf(queries->query9, BUFSIZE,
		"SELECT SQL_NO_CACHE snmp_port, count(snmp_port)"
		" FROM poller_item"
		" WHERE host_id = %i"
		" GROUP BY snmp_port %s", host_id, limits);

	/* number of agent's count for multiple polling intervals */
	if (set.active_profiles != 1) {
		snprintf(queries->query10, BUFSIZE,
			"SELECT SQL_NO_CACHE snmp_port, count(snmp_port)"
			" FROM poller_item"
			" WHERE host_id = %i"
			" AND rrd_next_step <= 0"
			" GROUP BY snmp_port %s", host_id, limits);
	} else {
		snprintf(queries->query10, BUFSIZE,
			"SELECT SQL_NO_CACHE snmp_port, count(snmp_port)"
			" FROM poller_item"
			" WHERE host_id = %i"
			" GROUP BY snmp_port %s", host_id, limits);
	}
}

static void build_collector_poll_queries(legacy_queries_t *queries, int host_id, const char *limits, const char *regex_col) {
	if (set.total_snmp_ports == 1) {
		snprintf(queries->query1, BUFSIZE,
			"SELECT SQL_NO_CACHE action, hostname, snmp_community, "
				"snmp_version, snmp_username, snmp_password, "
				"rrd_name, rrd_path, arg1, arg2, arg3, local_data_id, "
				"rrd_num, snmp_port, snmp_timeout, "
				"snmp_auth_protocol, snmp_priv_passphrase, snmp_priv_protocol, snmp_context, snmp_engine_id"
				"%s"
			" FROM poller_item"
			" WHERE host_id = %i"
			" AND poller_id=%i %s", regex_col, host_id, set.poller_id, limits);
	} else {
		snprintf(queries->query1, BUFSIZE,
			"SELECT SQL_NO_CACHE action, hostname, snmp_community, "
				"snmp_version, snmp_username, snmp_password, "
				"rrd_name, rrd_path, arg1, arg2, arg3, local_data_id, "
				"rrd_num, snmp_port, snmp_timeout, "
				"snmp_auth_protocol, snmp_priv_passphrase, snmp_priv_protocol, snmp_context, snmp_engine_id"
				"%s"
			" FROM poller_item"
			" WHERE host_id = %i"
			" AND poller_id=%i"
			" ORDER BY snmp_port %s", regex_col, host_id, set.poller_id, limits);
	}

	/* host structure for uptime checks */
	snprintf(queries->query2, BIG_BUFSIZE,
		"SELECT SQL_NO_CACHE id, hostname, snmp_community, snmp_version, "
			"snmp_username, snmp_password, snmp_auth_protocol, "
			"snmp_priv_passphrase, snmp_priv_protocol, snmp_context, snmp_engine_id, snmp_port, snmp_timeout, max_oids, "
			"availability_method, ping_method, ping_port, ping_timeout, ping_retries, "
			"status, status_event_count, UNIX_TIMESTAMP(status_fail_date), "
			"UNIX_TIMESTAMP(status_rec_date), status_last_error, "
			"min_time, max_time, cur_time, avg_time, "
			"total_polls, failed_polls, availability, snmp_sysUpTimeInstance, snmp_sysDescr, snmp_sysObjectID, "
			"snmp_sysContact, snmp_sysName, snmp_sysLocation"
		" FROM host"
		" WHERE id = %i"
		" AND deleted = ''", host_id);

	/* data query structure for reindex detection */
	snprintf(queries->query4, BUFSIZE,
		"SELECT SQL_NO_CACHE data_query_id, action, op, assert_value, arg1"
			" FROM poller_reindex"
			" WHERE host_id = %i", host_id);

	/* multiple polling interval query for items */
	if (set.active_profiles != 1) {
		if (set.total_snmp_ports == 1) {
			snprintf(queries->query5, BUFSIZE,
				"SELECT SQL_NO_CACHE action, hostname, snmp_community, "
					"snmp_version, snmp_username, snmp_password, "
					"rrd_name, rrd_path, arg1, arg2, arg3, local_data_id, "
					"rrd_num, snmp_port, snmp_timeout, "
					"snmp_auth_protocol, snmp_priv_passphrase, snmp_priv_protocol, snmp_context, snmp_engine_id"
					"%s"
				" FROM poller_item"
				" WHERE host_id = %i"
				" AND rrd_next_step <= 0"
				" AND poller_id = %i %s", regex_col, host_id, set.poller_id, limits);
		} else {
			snprintf(queries->query5, BUFSIZE,
				"SELECT SQL_NO_CACHE action, hostname, snmp_community, "
					"snmp_version, snmp_username, snmp_password, "
					"rrd_name, rrd_path, arg1, arg2, arg3, local_data_id, "
					"rrd_num, snmp_port, snmp_timeout, "
					"snmp_auth_protocol, snmp_priv_passphrase, snmp_priv_protocol, snmp_context, snmp_engine_id"
					"%s"
				" FROM poller_item"
				" WHERE host_id = %i"
				" AND rrd_next_step <= 0"
				" AND poller_id = %i"
				" ORDER BY snmp_port %s", regex_col, host_id, set.poller_id, limits);
		}
	} else {
		if (set.total_snmp_ports == 1) {
			snprintf(queries->query5, BUFSIZE,
				"SELECT SQL_NO_CACHE action, hostname, snmp_community, "
					"snmp_version, snmp_username, snmp_password, "
					"rrd_name, rrd_path, arg1, arg2, arg3, local_data_id, "
					"rrd_num, snmp_port, snmp_timeout, "
					"snmp_auth_protocol, snmp_priv_passphrase, snmp_priv_protocol, snmp_context, snmp_engine_id"
					"%s"
				" FROM poller_item"
				" WHERE host_id = %i"
				" AND poller_id = %i %s", regex_col, host_id, set.poller_id, limits);
		} else {
			snprintf(queries->query5, BUFSIZE,
				"SELECT SQL_NO_CACHE action, hostname, snmp_community, "
					"snmp_version, snmp_username, snmp_password, "
					"rrd_name, rrd_path, arg1, arg2, arg3, local_data_id, "
					"rrd_num, snmp_port, snmp_timeout, "
					"snmp_auth_protocol, snmp_priv_passphrase, snmp_priv_protocol, snmp_context, snmp_engine_id"
					"%s"
				" FROM poller_item"
				" WHERE host_id = %i"
				" AND poller_id = %i"
				" ORDER BY snmp_port %s", regex_col, host_id, set.poller_id, limits);
		}
	}

	/* query to add output records to the poller output table */
	snprintf(queries->query8, BUFSIZE,
		"INSERT INTO poller_output"
		" (local_data_id, rrd_name, time, output) VALUES");

	/* query suffix to add rows to the poller output table */
	snprintf(queries->posuffix, BUFSIZE,
		" ON DUPLICATE KEY UPDATE output=VALUES(output)");

	/* number of agent's count for single polling interval */
	snprintf(queries->query9, BUFSIZE,
		"SELECT SQL_NO_CACHE snmp_port, count(snmp_port)"
		" FROM poller_item"
		" WHERE host_id = %i"
		" AND poller_id = %i"
		" GROUP BY snmp_port %s", host_id, set.poller_id, limits);

	/* number of agent's count for multiple polling intervals */
	if (set.active_profiles != 1) {
		snprintf(queries->query10, BUFSIZE,
			"SELECT SQL_NO_CACHE snmp_port, count(snmp_port)"
			" FROM poller_item"
			" WHERE host_id = %i"
			" AND rrd_next_step <= 0"
			" AND poller_id = %i"
			" GROUP BY snmp_port %s", host_id, set.poller_id, limits);
	} else {
		snprintf(queries->query10, BUFSIZE,
			"SELECT SQL_NO_CACHE snmp_port, count(snmp_port)"
			" FROM poller_item"
			" WHERE host_id = %i"
			" AND poller_id = %i"
			" GROUP BY snmp_port %s", host_id, set.poller_id, limits);
	}
}

static void build_legacy_poll_queries(legacy_queries_t *queries, int host_id, int host_data_ids, int spine_host_thread) {
	char limits[SMALL_BUFSIZE];
	/* determine the SQL limits using the poller instructions */
	if (host_data_ids > 0) {
		snprintf(limits, SMALL_BUFSIZE, "LIMIT %i, %i", host_data_ids * (spine_host_thread - 1), host_data_ids);
	} else {
		limits[0] = '\0';
	}

	/* optional output_regex column (added in Cacti 1.3.1) */
	const char *regex_col = set.has_output_regex ? ", output_regex" : "";

	/* single polling interval query for items */
	if (set.poller_id == 0) {
		build_unscoped_poll_queries(queries, host_id, limits, regex_col);
	} else {
		build_collector_poll_queries(queries, host_id, limits, regex_col);
	}

	/* query to add output records to the poller output table */
	snprintf(queries->query11, BUFSIZE,
		"INSERT INTO poller_output_boost"
		" (local_data_id, rrd_name, time, output) VALUES");

	queries->query8_len   = strlen(queries->query8);
	queries->query11_len  = strlen(queries->query11);
	queries->posuffix_len = strlen(queries->posuffix);

}

static char *poll_reindex_uptime(spine_spine_host_t *host, const reindex_t *reindex,
	int spine_host_thread, char sysUptime[BUFSIZE]) {
	char *poll_result;
	int  uptime_use_engine_oid = FALSE;

     // Ensure uptime is empty to start with
     sysUptime[0] = '\0';

	/* Pin the uptime-goes-backward calculation to a single OID for this poll.
	   Previously the legacy (centisecond) and modern (second) OIDs could each
	   supply the value on different reindex rows or different poll cycles,
	   and a mismatch between the two caused false "uptime went backward"
	   detections and constant reindexing. Prefer the modern engine OID, since
	   it offers seconds granularity, and only fall back to the legacy OID
	   when the engine OID isn't present with numeric data. */
	poll_result = snmp_get_base(host, ".1.3.6.1.6.3.10.2.1.3.0", false);

	uptime_use_engine_oid = (poll_result != NULL && is_numeric(poll_result));

	if (uptime_use_engine_oid) {
		snprintf(sysUptime, BUFSIZE, "%lld", atoll(poll_result) * 100);
	}

	SPINE_FREE(poll_result);

	if (is_debug_device(host->id)) {
		SPINE_LOG(("Device[%i] HT[%i] DQ[%i] Engine Uptime OID Present: %d", host->id, spine_host_thread, reindex->data_query_id, uptime_use_engine_oid));
	} else {
		SPINE_LOG_MEDIUM(("Device[%i] HT[%i] DQ[%i] Engine Uptime OID Present: %d", host->id, spine_host_thread, reindex->data_query_id, uptime_use_engine_oid));
	}

	if (!uptime_use_engine_oid) {
		// Engine OID unavailable, fall back to the legacy sysUpTime OID
		poll_result = snmp_get(host, ".1.3.6.1.2.1.1.3.0");

		if (poll_result && is_numeric(poll_result)) {
			snprintf(sysUptime, BUFSIZE, "%s", poll_result);
		}

		if (is_debug_device(host->id)) {
			SPINE_LOG(("Device[%i] HT[%i] DQ[%i] Legacy Uptime Result: %s, Is Numeric: %d", host->id, spine_host_thread, reindex->data_query_id, poll_result, is_numeric(poll_result) ));
		} else {
			SPINE_LOG_MEDIUM(("Device[%i] HT[%i] DQ[%i] Legacy Uptime Result: %s, Is Numeric: %d", host->id, spine_host_thread, reindex->data_query_id, poll_result, is_numeric(poll_result) ));
		}

		SPINE_FREE(poll_result);
	}

	/* allocate and populate with whichever uptime was valid */
	if (!(poll_result = (char *) malloc(BUFSIZE))) {
		die("ERROR: Fatal malloc error: poller.c poll_result");
	}
	snprintf(poll_result, BUFSIZE, "%s", sysUptime);

	if (is_debug_device(host->id)) {
		SPINE_LOG(("Device[%i] HT[%i] DQ[%i] Extended Uptime Result: %s, Is Numeric: %d", host->id, spine_host_thread, reindex->data_query_id, poll_result, is_numeric(poll_result) ));
	} else {
		SPINE_LOG_MEDIUM(("Device[%i] HT[%i] DQ[%i] Extended Uptime Result: %s, Is Numeric: %d", host->id, spine_host_thread, reindex->data_query_id, poll_result, is_numeric(poll_result) ));
	}
	return poll_result;
}

static char *poll_reindex_snmp(spine_spine_host_t *host, const reindex_t *reindex,
	int spine_host_thread, char sysUptime[BUFSIZE], int *reindex_err) {
	char *poll_result = NULL;
	/* if there is no snmp session, don't probe */
	if (host->snmp_session == NULL) {
		(*reindex_err) = TRUE;
	}

	/* check to see if you are checking uptime */
	if (!(*reindex_err)) {
		if ((strstr(reindex->arg1, ".1.3.6.1.2.1.1.3.0") ||
			strstr(reindex->arg1, ".1.3.6.1.6.3.10.2.1.3.0")) && strlen(sysUptime) > 0) {

			if (!(poll_result = (char *) malloc(BUFSIZE))) {
				die("ERROR: Fatal malloc error: poller.c poll_result");
			}

			poll_result[0] = '\0';

			snprintf(poll_result, BUFSIZE, "%s", sysUptime);
		} else if (strstr(reindex->arg1, ".1.3.6.1.2.1.1.3.0") ||
			strstr(reindex->arg1, ".1.3.6.1.6.3.10.2.1.3.0")) {
			poll_result = poll_reindex_uptime(host, reindex, spine_host_thread, sysUptime);
		} else {
			poll_result = snmp_get(host, reindex->arg1);
		}

		if (is_debug_device(host->id)) {
			SPINE_LOG(("Device[%i] HT[%i] DQ[%i] RECACHE OID: %s, (assert: %s %s output: %s)", host->id, spine_host_thread, reindex->data_query_id, reindex->arg1, reindex->assert_value, reindex->op, poll_result));
		} else {
			SPINE_LOG_MEDIUM(("Device[%i] HT[%i] DQ[%i] RECACHE OID: %s, (assert: %s %s output: %s)", host->id, spine_host_thread, reindex->data_query_id, reindex->arg1, reindex->assert_value, reindex->op, poll_result));
		}
	} else {
		SPINE_LOG(("WARNING: Device[%i] HT[%i] DQ[%i] Reindex Check FAILED: No SNMP Session.  If not an SNMP host, don't use Uptime Goes Backwards!", host->id, spine_host_thread, reindex->data_query_id));
	}

	return poll_result;
}

static char *poll_reindex_script_count(spine_spine_host_t *host, reindex_t *reindex,
	int spine_host_thread) {
	char *poll_result;
	if (!(poll_result = (char *) malloc(BUFSIZE))) {
		die("ERROR: Fatal malloc error: poller.c poll_result");
	}
	poll_result[0] = '\0';

	{
		char *ep_result = exec_poll(host, reindex->arg1, reindex->data_query_id, "DQ");
		snprintf(poll_result, BUFSIZE, "%d", char_count(ep_result, '\n'));
		free(ep_result);
	}

	if (is_debug_device(host->id)) {
		SPINE_LOG(("Device[%i] HT[%i] DQ[%i] RECACHE CMD COUNT: %s, output: %s", host->id, spine_host_thread, reindex->data_query_id, reindex->arg1, poll_result));
	} else {
		SPINE_LOG_MEDIUM(("Device[%i] HT[%i] DQ[%i] RECACHE CMD COUNT: %s, output: %s", host->id, spine_host_thread, reindex->data_query_id, reindex->arg1, poll_result));
	}
	return poll_result;
}

static char *poll_reindex_source(spine_spine_host_t *host, reindex_t *reindex,
	int spine_host_thread, char sysUptime[BUFSIZE], int *reindex_err) {
	char *poll_result = NULL;
	int php_process;
	switch(reindex->action) {
	case POLLER_ACTION_SNMP: /* snmp */
		poll_result = poll_reindex_snmp(host, reindex, spine_host_thread, sysUptime, reindex_err);
		break;
	case POLLER_ACTION_SCRIPT: /* script (popen) */
		/* Reject empty script commands that could cause unexpected behavior */
		if (strlen(reindex->arg1) == 0) {
			SPINE_LOG(("WARNING: Device[%i] HT[%i] DQ[%i] empty script command, skipping",
				host->id, spine_host_thread, reindex->data_query_id));
			break;
		}

		poll_result = trim(exec_poll(host, reindex->arg1, reindex->data_query_id, "DQ"));

		if (is_debug_device(host->id)) {
			SPINE_LOG(("Device[%i] HT[%i] DQ[%i] RECACHE CMD: %s, output: %s", host->id, spine_host_thread, reindex->data_query_id, reindex->arg1, poll_result));
		} else {
			SPINE_LOG_MEDIUM(("Device[%i] HT[%i] DQ[%i] RECACHE CMD: %s, output: %s", host->id, spine_host_thread, reindex->data_query_id, reindex->arg1, poll_result));
		}

		break;
	case POLLER_ACTION_PHP_SCRIPT_SERVER: /* script (php script server) */
		php_process = php_get_process();

		poll_result = trim(php_cmd(reindex->arg1, php_process));

		if (is_debug_device(host->id)) {
			SPINE_LOG(("Device[%i] HT[%i] DQ[%i] RECACHE SERVER: %s, output: %s", host->id, spine_host_thread, reindex->data_query_id, reindex->arg1, poll_result));
		} else {
			SPINE_LOG_MEDIUM(("Device[%i] HT[%i] DQ[%i] RECACHE SERVER: %s, output: %s", host->id, spine_host_thread, reindex->data_query_id, reindex->arg1, poll_result));
		}

		break;
	case POLLER_ACTION_SNMP_COUNT: { /* snmp; count items */
		int snmp_items;

		if (!(poll_result = (char *) malloc(BUFSIZE))) {
			die("ERROR: Fatal malloc error: poller.c poll_result");
		}
		poll_result[0] = '\0';

		snmp_items = snmp_count(host, reindex->arg1);
		if (snmp_items < 0) {
			SET_UNDEFINED(poll_result);
		} else {
			snprintf(poll_result, BUFSIZE, "%d", snmp_items);
		}

		if (is_debug_device(host->id)) {
			SPINE_LOG(("Device[%i] HT[%i] DQ[%i] RECACHE OID COUNT: %s, output: %s", host->id, spine_host_thread, reindex->data_query_id, reindex->arg1, poll_result));
		} else {
			SPINE_LOG_MEDIUM(("Device[%i] HT[%i] DQ[%i] RECACHE OID COUNT: %s, output: %s", host->id, spine_host_thread, reindex->data_query_id, reindex->arg1, poll_result));
		}

		break;
	}
	case POLLER_ACTION_SCRIPT_COUNT: /* script (popen); count items by counting line feeds */
		poll_result = poll_reindex_script_count(host, reindex, spine_host_thread);
		break;
	case POLLER_ACTION_PHP_SCRIPT_SERVER_COUNT: /* script (php script server); count number of lines */
		if (!(poll_result = (char *) malloc(BUFSIZE))) {
			die("ERROR: Fatal malloc error: poller.c poll_result");
		}
		poll_result[0] = '\0';

		php_process = php_get_process();

		{
			char *php_result = php_cmd(reindex->arg1, php_process);
			snprintf(poll_result, BUFSIZE, "%d", char_count(php_result, '\n'));
			free(php_result);
		}

		if (is_debug_device(host->id)) {
			SPINE_LOG(("Device[%i] HT[%i] DQ[%i] RECACHE SERVER COUNT: %s, output: %s", host->id, spine_host_thread, reindex->data_query_id, reindex->arg1, poll_result));
		} else {
			SPINE_LOG_MEDIUM(("Device[%i] HT[%i] DQ[%i] RECACHE SERVER COUNT: %s, output: %s", host->id, spine_host_thread, reindex->data_query_id, reindex->arg1, poll_result));
		}

		break;
	default:
		SPINE_LOG(("Device[%i] HT[%i] ERROR: Unknown Assert Action!", host->id, spine_host_thread));
	}
	return poll_result;
}

typedef struct {
	int host_id;
	int spine_host_thread;
	int *assert_fail;
	int *previous_assert_failure;
	int *errors;
	int *spike_kill;
	MYSQL *local;
	MYSQL *remote;
} reindex_assertion_t;

static void classify_reindex_assertion(const spine_spine_host_t *host, const reindex_assertion_t *state) {
	if (is_debug_device(host->id) || set.spine_log_level == 2) {
		return;
	}
	if (set.spine_log_level == 1) {
		(*state->errors)++;
	}
}

static void enqueue_reindex_command(const reindex_assertion_t *state, const char *query3) {
	if (state->spine_host_thread == 1) {
		if (set.poller_id > 1 && set.mode == REMOTE_ONLINE) {
			db_insert(state->remote, REMOTE, query3);
		} else {
			db_insert(state->local, LOCAL, query3);
		}
	}
	(*state->assert_fail) = TRUE;
	(*state->previous_assert_failure) = TRUE;
}

static void queue_changed_reindex(const spine_spine_host_t *host, reindex_t *reindex, const reindex_assertion_t *state, const char *poll_result, char *query3) {
	classify_reindex_assertion(host, state);
	SPINE_LOG(("Device[%i] HT[%i] DQ[%i] RECACHE ASSERT FAILED: '%s=%s'", host->id, state->spine_host_thread, reindex->data_query_id, reindex->assert_value, poll_result));
	if (state->spine_host_thread == 1) {
		snprintf(query3, LRG_BUFSIZE, "REPLACE INTO poller_command (poller_id, time, action,command) values (%i, NOW(), %i, '%i:%i')", set.poller_id, POLLER_COMMAND_REINDEX, host->id, reindex->data_query_id);
	}
	enqueue_reindex_command(state, query3);
}

static void queue_increasing_reindex(const spine_spine_host_t *host, reindex_t *reindex, const reindex_assertion_t *state, const char *poll_result, char *query3) {
	classify_reindex_assertion(host, state);
	SPINE_LOG(("Device[%i] HT[%i] DQ[%i] RECACHE ASSERT FAILED: '%s>%s'", host->id, state->spine_host_thread, reindex->data_query_id, reindex->assert_value, poll_result));
	if (state->spine_host_thread == 1) {
		snprintf(query3, LRG_BUFSIZE, "REPLACE INTO poller_command (poller_id, time, action, command) ValueS (%i, NOW(), %i, '%i:%i')", set.poller_id, POLLER_COMMAND_REINDEX, host->id, reindex->data_query_id);
	}
	enqueue_reindex_command(state, query3);
}

static void queue_decreasing_reindex(const spine_spine_host_t *host, reindex_t *reindex, const reindex_assertion_t *state, const char *poll_result, char *query3) {
	classify_reindex_assertion(host, state);
	SPINE_LOG(("Device[%i] HT[%i] DQ[%i] RECACHE ASSERT FAILED: '%s<%s'", host->id, state->spine_host_thread, reindex->data_query_id, reindex->assert_value, poll_result));
	if (state->spine_host_thread == 1) {
		snprintf(query3, LRG_BUFSIZE, "REPLACE INTO poller_command (poller_id, time, action, command) VALUES (%i, NOW(), %i, '%i:%i')", set.poller_id, POLLER_COMMAND_REINDEX, host->id, reindex->data_query_id);
	}
	enqueue_reindex_command(state, query3);
}

static void record_reindex_spike(spine_spine_host_t *host, const reindex_assertion_t *state) {
	(*state->spike_kill) = TRUE;

	if (is_debug_device(host->id) || set.spine_log_level == 2) {
		SPINE_LOG(("Device[%i] HT[%i] NOTICE: Spike Kill in Effect for '%s'", state->host_id, state->spine_host_thread, host->hostname));
	} else {
		if (set.spine_log_level == 1) {
			(*state->errors)++;
		}

		SPINE_LOG_MEDIUM(("Device[%i] HT[%i] NOTICE: Spike Kill in Effect for '%s'", state->host_id, state->spine_host_thread, host->hostname));
	}
}

static void persist_reindex_assertion(spine_spine_host_t *host, const reindex_t *reindex,
	const reindex_assertion_t *state, const char *poll_result, char *query3) {
	char temp_poll_result[BUFSIZE];
	char temp_arg1[BUFSIZE];
	/* update 'poller_reindex' with the correct information if:
	 * 1) the assert fails
	 * 2) the OP code is > or < meaning the current value could have changed without causing
	 *     the assert to fail */
	if ((*state->assert_fail) || (!strcmp(reindex->op, ">")) || (!strcmp(reindex->op, "<"))) {
		if (state->spine_host_thread == 1) {
			db_escape(state->local, temp_poll_result, sizeof(temp_poll_result), poll_result);
			db_escape(state->local, temp_arg1, sizeof(temp_arg1), reindex->arg1);

			snprintf(query3, LRG_BUFSIZE, "UPDATE poller_reindex SET assert_value='%s' WHERE host_id='%i' AND data_query_id='%i' AND arg1='%s'", temp_poll_result, state->host_id, reindex->data_query_id, temp_arg1);

			db_insert(state->local, LOCAL, query3);

		}

		if ((*state->assert_fail) &&
			((!strcmp(reindex->op, "<")) || (!strcmp(reindex->arg1,".1.3.6.1.2.1.1.3.0") || !strcmp(reindex->arg1, ".1.3.6.1.6.3.10.2.1.3.0")))) {
			record_reindex_spike(host, state);
		}
	}
}

static void evaluate_reindex_result(spine_spine_host_t *host, reindex_t *reindex,
	const reindex_assertion_t *state, char *poll_result) {
	char *query3 = NULL;

	if (!(query3 = (char *)malloc(LRG_BUFSIZE))) {
		die("ERROR: Fatal malloc error: poller.c reindex insert!");
	}
	query3[0] = '\0';

	/* assume ok if host is up and result wasn't obtained */
	if (poll_result == NULL || (IS_UNDEFINED(poll_result)) || (STRIMATCH(poll_result, "No Such Instance"))) {
		if (is_debug_device(host->id) || set.spine_log_level == 2) {
			SPINE_LOG(("Device[%i] HT[%i] DQ[%i] RECACHE ASSERT FAILED: '%s=%s'", host->id, state->spine_host_thread, reindex->data_query_id, reindex->assert_value, poll_result));
		}

		(*state->assert_fail) = FALSE;
	} else if ((!strcmp(reindex->op, "=")) && (strcmp(reindex->assert_value, poll_result))) {
		queue_changed_reindex(host, reindex, state, poll_result, query3);
	} else if ((!strcmp(reindex->op, ">")) && (atoll(reindex->assert_value) < atoll(poll_result))) {
		queue_increasing_reindex(host, reindex, state, poll_result, query3);
	} else if (strcmp(reindex->assert_value, "0")) {
		if ((!strcmp(reindex->op, "<")) && (atoll(reindex->assert_value) > atoll(poll_result))) {
			queue_decreasing_reindex(host, reindex, state, poll_result, query3);
		}
	}

	persist_reindex_assertion(host, reindex, state, poll_result, query3);

	SPINE_FREE(query3);
	SPINE_FREE(poll_result);
}

/* Borrow result arrays and counters only for the current native poll.
 * Preserve the distinct profile-switch, full-batch, and final-batch rules. */
typedef struct {
	int host_id;
	int spine_host_thread;
	char *error_string;
	int *buf_size;
	int *buf_errors;
	int *errors;
	double *thread_start;
	double *thread_end;
	int *spike_kill;
} legacy_result_context_t;

static void report_invalid_snmp_result(const spine_spine_host_t *host, const target_t *poller_items, const snmp_oids_t *snmp_oids, int j, const legacy_result_context_t *context) {
	buffer_output_errors(context->error_string, context->buf_size, context->buf_errors, context->host_id, context->spine_host_thread, poller_items[snmp_oids[j].array_position].local_data_id, false);
	(*context->errors)++;

	if (set.spine_log_level == 2) {
		SPINE_LOG(("WARNING: Invalid Response, Device[%i] HT[%i] DS[%i] SNMP: v%i: %s, dsname: %s, oid: %s, value: %s",
			context->host_id, context->spine_host_thread, poller_items[snmp_oids[j].array_position].local_data_id,
			host->snmp_version, host->hostname, poller_items[snmp_oids[j].array_position].rrd_name,
			poller_items[snmp_oids[j].array_position].arg1, snmp_oids[j].result));
	}
}

static void normalize_snmp_result(const spine_spine_host_t *host, const target_t *poller_items, snmp_oids_t *snmp_oids, int j, const legacy_result_context_t *context) {
	char temp_result[RESULTS_BUFFER];
	if (host->ignore_host) {
		SPINE_LOG(("Device[%i] HT[%i] DS[%i] WARNING: SNMP timeout detected [%i ms], ignoring host '%s'", context->host_id, context->spine_host_thread, poller_items[snmp_oids[j].array_position].local_data_id, host->snmp_timeout, host->hostname));
		SET_UNDEFINED(snmp_oids[j].result);
	} else if (IS_UNDEFINED(snmp_oids[j].result)) {
		report_invalid_snmp_result(host, poller_items, snmp_oids, j, context);

		/* continue */
	} else if ((is_numeric(snmp_oids[j].result)) || (is_multipart_output(snmp_oids[j].result))) {
		/* continue */
	} else if (is_hexadecimal(snmp_oids[j].result, TRUE)) {
		if (!poller_store_hex_result(snmp_oids[j].result, RESULTS_BUFFER, snmp_oids[j].result, context->errors)) {
			buffer_output_errors(context->error_string, context->buf_size, context->buf_errors, context->host_id, context->spine_host_thread, poller_items[snmp_oids[j].array_position].local_data_id, false);
			if (set.spine_log_level == 2) {
				SPINE_LOG(("WARNING: Hexadecimal Response Exceeds 64 Bits, Device[%i] HT[%i] DS[%i]", context->host_id, context->spine_host_thread, poller_items[snmp_oids[j].array_position].local_data_id));
			}
		}
	} else if ((STRIMATCH(snmp_oids[j].result, "U")) ||
		(STRIMATCH(snmp_oids[j].result, "Nan"))) {
		report_invalid_snmp_result(host, poller_items, snmp_oids, j, context);

		/* is valid output, continue */
	} else {
		/* trim a non-numeric prefix or suffix, then validate below */
		snprintf(temp_result, RESULTS_BUFFER, "%s", strip_alpha(snmp_oids[j].result));
		snprintf(snmp_oids[j].result , RESULTS_BUFFER, "%s", temp_result);

		/* detect erroneous non-numeric result */
		if (!validate_result(snmp_oids[j].result)) {
			report_invalid_snmp_result(host, poller_items, snmp_oids, j, context);

			SET_UNDEFINED(snmp_oids[j].result);
		}
	}

}

static void store_snmp_result(const spine_spine_host_t *host, target_t *poller_items, const snmp_oids_t *snmp_oids, int j, const legacy_result_context_t *context) {
	snprintf(poller_items[snmp_oids[j].array_position].result, RESULTS_BUFFER, "%s", snmp_oids[j].result);

	(*context->thread_end) = get_time_as_double();

	if (is_debug_device(context->host_id)) {
		SPINE_LOG(("Device[%i] HT[%i] DS[%i] TT[%.2f] SNMP: v%i: %s, dsname: %s, oid: %s, value: %s", context->host_id, context->spine_host_thread, poller_items[snmp_oids[j].array_position].local_data_id, (float) (((*context->thread_end) - (*context->thread_start)) * 1000), host->snmp_version, host->hostname, poller_items[snmp_oids[j].array_position].rrd_name, poller_items[snmp_oids[j].array_position].arg1, poller_items[snmp_oids[j].array_position].result));
	} else {
		SPINE_LOG_MEDIUM(("Device[%i] HT[%i] DS[%i] TT[%.2f] SNMP: v%i: %s, dsname: %s, oid: %s, value: %s", context->host_id, context->spine_host_thread, poller_items[snmp_oids[j].array_position].local_data_id, (float) (((*context->thread_end) - (*context->thread_start)) * 1000), host->snmp_version, host->hostname, poller_items[snmp_oids[j].array_position].rrd_name, poller_items[snmp_oids[j].array_position].arg1, poller_items[snmp_oids[j].array_position].result));
	}
}

static void kill_legacy_scalar_spike(target_t *item, const legacy_result_context_t *context) {
	if (!IS_UNDEFINED(item->result)) {
		/* insert a NaN in place of the actual value if the snmp agent restarts */
		if ((*context->spike_kill) && (!strstr(item->result,":"))) {
			SET_UNDEFINED(item->result);
		}
	}
}

static void consume_profile_switch_result(const spine_spine_host_t *host, target_t *poller_items, snmp_oids_t *snmp_oids, int j, const legacy_result_context_t *context) {
	normalize_snmp_result(host, poller_items, snmp_oids, j, context);
	store_snmp_result(host, poller_items, snmp_oids, j, context);
}

static void consume_full_batch_result(const spine_spine_host_t *host, target_t *poller_items, snmp_oids_t *snmp_oids, int j, const legacy_result_context_t *context) {
	char temp_result[RESULTS_BUFFER];
	normalize_snmp_result(host, poller_items, snmp_oids, j, context);

	if (strlen(poller_items[snmp_oids[j].array_position].output_regex)) {
		snprintf(temp_result, RESULTS_BUFFER, "%s", regex_replace(poller_items[snmp_oids[j].array_position].output_regex, snmp_oids[j].result));
		snprintf(snmp_oids[j].result, RESULTS_BUFFER, "%s", temp_result);
	}

	store_snmp_result(host, poller_items, snmp_oids, j, context);
	kill_legacy_scalar_spike(&poller_items[snmp_oids[j].array_position], context);
}

static void consume_final_batch_result(const spine_spine_host_t *host, target_t *poller_items, snmp_oids_t *snmp_oids, int j, const legacy_result_context_t *context) {
	char temp_result[RESULTS_BUFFER];
	normalize_snmp_result(host, poller_items, snmp_oids, j, context);

	if (strlen(poller_items[snmp_oids[j].array_position].output_regex)) {
		snprintf(temp_result, RESULTS_BUFFER, "%s", regex_replace(poller_items[snmp_oids[j].array_position].output_regex, snmp_oids[j].result));
		snprintf(snmp_oids[j].result, RESULTS_BUFFER, "%s", temp_result);
	}

	store_snmp_result(host, poller_items, snmp_oids, j, context);
	kill_legacy_scalar_spike(&poller_items[snmp_oids[j].array_position], context);
}

static void initialize_legacy_host(spine_spine_host_t *host) {
	/* initialize variables first */
	host->id                      = 0;                 // 0
	host->hostname[0]             = '\0';              // 1
	host->snmp_session            = NULL;              // -
	host->snmp_community[0]       = '\0';              // 2
	host->snmp_version            = 1;                 // 3
	host->snmp_username[0]        = '\0';              // 4
	host->snmp_password[0]        = '\0';              // 5
	host->snmp_auth_protocol[0]   = '\0';              // 6
	host->snmp_priv_passphrase[0] = '\0';              // 7
	host->snmp_priv_protocol[0]   = '\0';              // 8
	host->snmp_context[0]         = '\0';              // 9
	host->snmp_engine_id[0]       = '\0';              // 10
	host->snmp_port               = 161;               // 11
	host->snmp_timeout            = 500;               // 12
	host->snmp_retries            = set.snmp_retries;  // -
	host->max_oids                = 10;                // 13
	host->availability_method     = 0;                 // 14
	host->ping_method             = 0;                 // 15
	host->ping_port               = 23;                // 16
	host->ping_timeout            = 500;               // 17
	host->ping_retries            = 2;                 // 18
	host->status                  = HOST_UP;           // 19
	host->status_event_count      = 0;                 // 20
	host->status_fail_date[0]     = '\0';              // 21
	host->status_rec_date[0]      = '\0';              // 22
	host->status_last_error[0]    = '\0';              // 23
	host->min_time                = 0;                 // 24
	host->max_time                = 0;                 // 25
	host->cur_time                = 0;                 // 26
	host->avg_time                = 0;                 // 27
	host->total_polls             = 0;                 // 28
	host->failed_polls            = 0;                 // 29
	host->availability            = 100;               // 30
	host->snmp_sysUpTimeInstance  = 0;                 // 31
	host->snmp_sysDescr[0]        = '\0';              // 32
	host->snmp_sysObjectID[0]     = '\0';              // 33
	host->snmp_sysContact[0]      = '\0';              // 34
	host->snmp_sysName[0]         = '\0';              // 35
	host->snmp_sysLocation[0]     = '\0';              // 36

}

static void load_legacy_host_snmp(spine_spine_host_t *host, MYSQL_ROW row) {
	name_t *name;
	/* populate host structure */
	host->ignore_host = FALSE;
	if (row[0]  != NULL) host->id = atoi(row[0]);

	if (row[1]  != NULL) {
		name = get_namebyhost(row[1], NULL);
		STRNCOPY(host->hostname, name->hostname);
		host->ping_port = name->port;
		SPINE_FREE(name);
	}

	if (row[2]  != NULL) STRNCOPY(host->snmp_community,       row[2]);

	if (row[3]  != NULL) host->snmp_version = atoi(row[3]);

	if (row[4]  != NULL) STRNCOPY(host->snmp_username,        row[4]);
	if (row[5]  != NULL) STRNCOPY(host->snmp_password,        row[5]);
	if (row[6]  != NULL) STRNCOPY(host->snmp_auth_protocol,   row[6]);
	if (row[7]  != NULL) STRNCOPY(host->snmp_priv_passphrase, row[7]);
	if (row[8]  != NULL) STRNCOPY(host->snmp_priv_protocol,   row[8]);
	if (row[9]  != NULL) STRNCOPY(host->snmp_context,         row[9]);
	if (row[10]  != NULL) STRNCOPY(host->snmp_engine_id,       row[10]);

	if (row[11] != NULL) host->snmp_port           = atoi(row[11]);
	if (row[12] != NULL) host->snmp_timeout        = atoi(row[12]);
	if (row[13] != NULL) host->max_oids            = atoi(row[13]);

}

static void load_legacy_host_availability(spine_spine_host_t *host, MYSQL_ROW row) {
	if (row[14] != NULL) host->availability_method = atoi(row[14]);
	if (row[15] != NULL) host->ping_method         = atoi(row[15]);
	if (row[16] != NULL) host->ping_port           = atoi(row[16]);
	if (row[17] != NULL) host->ping_timeout        = atoi(row[17]);
	if (row[18] != NULL) host->ping_retries        = atoi(row[18]);

	if (row[19] != NULL) host->status              = atoi(row[19]);
	if (row[20] != NULL) host->status_event_count  = atoi(row[20]);

	if (row[21] != NULL) STRNCOPY(host->status_fail_date, row[21]);
	if (row[22] != NULL) STRNCOPY(host->status_rec_date,  row[22]);

	if (row[23] != NULL) STRNCOPY(host->status_last_error, row[23]);

	if (row[24] != NULL) host->min_time     = atof(row[24]);
	if (row[25] != NULL) host->max_time     = atof(row[25]);
	if (row[26] != NULL) host->cur_time     = atof(row[26]);
	if (row[27] != NULL) host->avg_time     = atof(row[27]);
	if (row[28] != NULL) host->total_polls  = atoi(row[28]);
	if (row[29] != NULL) host->failed_polls = atoi(row[29]);
	if (row[30] != NULL) host->availability = atof(row[30]);

}

static void load_legacy_host_system(spine_spine_host_t *host, MYSQL_ROW row, MYSQL *mysql) {
	if (row[31] != NULL) host->snmp_sysUpTimeInstance=atoll(row[31]);
	if (row[32] != NULL) db_escape(mysql, host->snmp_sysDescr, sizeof(host->snmp_sysDescr), row[32]);
	if (row[33] != NULL) db_escape(mysql, host->snmp_sysObjectID, sizeof(host->snmp_sysObjectID), row[33]);
	if (row[34] != NULL) db_escape(mysql, host->snmp_sysContact, sizeof(host->snmp_sysContact), row[34]);
	if (row[35] != NULL) db_escape(mysql, host->snmp_sysName, sizeof(host->snmp_sysName), row[35]);
	if (row[36] != NULL) db_escape(mysql, host->snmp_sysLocation, sizeof(host->snmp_sysLocation), row[36]);

}

static void refresh_legacy_system_information(spine_spine_host_t *host, MYSQL *mysql, int *ignore_sysinfo) {
	if ((host->availability_method != AVAIL_PING) && (host->availability_method != AVAIL_NONE)) {
		if (host->snmp_session != NULL && set.mibs) {
			get_system_information(host, mysql, 1);
			*ignore_sysinfo = FALSE;
		}
	}
}

static void check_legacy_host_availability(spine_spine_host_t *host, ping_t *ping, MYSQL *mysql, int spine_host_thread, int *ignore_sysinfo) {
	/* perform a check to see if the host is alive by polling it's SysDesc
	 * if the host down from an snmp perspective, don't poll it.
	 * function sets the ignore_host bit */
	if ((host->availability_method == AVAIL_SNMP) &&
		(strlen(host->snmp_community) == 0) &&
		(host->snmp_version < 3)) {
		host->ignore_host = FALSE;
		update_host_status(HOST_UP, host, ping, host->availability_method);

		if (is_debug_device(host->id)) {
			SPINE_LOG(("Device[%i] HT[%i] No host availability check possible for '%s'", host->id, spine_host_thread, host->hostname));
		} else {
			SPINE_LOG_MEDIUM(("Device[%i] HT[%i] No host availability check possible for '%s'", host->id, spine_host_thread, host->hostname));
		}
	} else if (host->availability_method == AVAIL_STREAM) {
		update_host_status(HOST_UP, host, ping, host->availability_method);
	} else {
		if (ping_host(host, ping) == HOST_UP) {
			host->ignore_host = FALSE;
			if (spine_host_thread == 1) {
				update_host_status(HOST_UP, host, ping, host->availability_method);

				refresh_legacy_system_information(host, mysql, ignore_sysinfo);
			}
		} else {
			host->ignore_host = TRUE;
			if (spine_host_thread == 1) {
				update_host_status(HOST_DOWN, host, ping, host->availability_method);
			}
		}
	}

}

static void persist_legacy_host_status(const spine_spine_host_t *host, MYSQL *mysql, int spine_host_thread, int ignore_sysinfo) {
	char update_sql[BIG_BUFSIZE];
	/* update host table */
	if (spine_host_thread == 1) {
		char escaped_last_error[BUFSIZE];
		db_escape(mysql, escaped_last_error, sizeof(escaped_last_error), host->status_last_error);

		if (!ignore_sysinfo) {
			if (host->ignore_host != TRUE) {
				snprintf(update_sql, BIG_BUFSIZE, "UPDATE host "
					"SET status='%i', status_event_count='%i', status_fail_date=FROM_UNIXTIME(%s),"
						" status_rec_date=FROM_UNIXTIME(%s), status_last_error='%s', min_time='%f',"
						" max_time='%f', cur_time='%f', avg_time='%f', total_polls='%i',"
						" failed_polls='%i', availability='%.4f', snmp_sysDescr='%s', "
						" snmp_sysObjectID='%s', snmp_sysUpTimeInstance='%llu', "
						" snmp_sysContact='%s', snmp_sysName='%s', snmp_sysLocation='%s' "
					"WHERE id='%i'",
					host->status,
					host->status_event_count,
					host->status_fail_date,
					host->status_rec_date,
					escaped_last_error,
					host->min_time,
					host->max_time,
					host->cur_time,
					host->avg_time,
					host->total_polls,
					host->failed_polls,
					host->availability,
					host->snmp_sysDescr,
					host->snmp_sysObjectID,
					host->snmp_sysUpTimeInstance,
					host->snmp_sysContact,
					host->snmp_sysName,
					host->snmp_sysLocation,
					host->id);
			} else {
				snprintf(update_sql, BIG_BUFSIZE, "UPDATE host "
					"SET status='%i', status_event_count='%i', status_fail_date=FROM_UNIXTIME(%s),"
						" status_rec_date=FROM_UNIXTIME(%s), status_last_error='%s', min_time='%f',"
						" max_time='%f', cur_time='%f', avg_time='%f', total_polls='%i',"
						" failed_polls='%i', availability='%.4f' "
					"WHERE id='%i'",
					host->status,
					host->status_event_count,
					host->status_fail_date,
					host->status_rec_date,
					escaped_last_error,
					host->min_time,
					host->max_time,
					host->cur_time,
					host->avg_time,
					host->total_polls,
					host->failed_polls,
					host->availability,
					host->id);
			}
		} else {
			snprintf(update_sql, BIG_BUFSIZE, "UPDATE host "
				"SET status='%i', status_event_count='%i', status_fail_date=FROM_UNIXTIME(%s),"
					" status_rec_date=FROM_UNIXTIME(%s), status_last_error='%s', min_time='%f',"
					" max_time='%f', cur_time='%f', avg_time='%f', total_polls='%i',"
					" failed_polls='%i', availability='%.4f' "
				"WHERE id='%i'",
				host->status,
				host->status_event_count,
				host->status_fail_date,
				host->status_rec_date,
				escaped_last_error,
				host->min_time,
				host->max_time,
				host->cur_time,
				host->avg_time,
				host->total_polls,
				host->failed_polls,
				host->availability,
				host->id);
		}

		db_insert(mysql, LOCAL, update_sql);
	}
}

static void report_invalid_script_result(const target_t *poller_items, int i, const legacy_result_context_t *context) {
	buffer_output_errors(context->error_string, context->buf_size, context->buf_errors, context->host_id, context->spine_host_thread, poller_items[i].local_data_id, false);
	(*context->errors)++;

	if (set.spine_log_level == 2) {
		SPINE_LOG(("WARNING: Invalid Response, Device[%i] HT[%i] DS[%i] SCRIPT: %s, output: %s",
			context->host_id, context->spine_host_thread, poller_items[i].local_data_id,
			poller_items[i].arg1, poller_items[i].result));
	}
}

static void normalize_script_result(target_t *poller_items, int i, char *poll_result, const legacy_result_context_t *context) {
	char temp_result[RESULTS_BUFFER];
	if (IS_UNDEFINED(poll_result)) {
		SET_UNDEFINED(poller_items[i].result);
		report_invalid_script_result(poller_items, i, context);
	} else if ((is_numeric(poll_result)) || (is_multipart_output(trim(poll_result)))) {
		snprintf(poller_items[i].result, RESULTS_BUFFER, "%s", poll_result);
	} else if (is_hexadecimal(poll_result, TRUE)) {
		if (!poller_store_hex_result(poller_items[i].result, RESULTS_BUFFER, poll_result, context->errors)) {
			buffer_output_errors(context->error_string, context->buf_size, context->buf_errors, context->host_id, context->spine_host_thread, poller_items[i].local_data_id, false);
			if (set.spine_log_level == 2) {
				SPINE_LOG(("WARNING: Hexadecimal Response Exceeds 64 Bits, Device[%i] HT[%i] DS[%i] SCRIPT: %s", context->host_id, context->spine_host_thread, poller_items[i].local_data_id, poller_items[i].arg1));
			}
		}
	} else {
		/* trim a non-numeric prefix or suffix, then validate below */
		snprintf(temp_result, RESULTS_BUFFER, "%s", strip_alpha(poll_result));
		snprintf(poller_items[i].result , RESULTS_BUFFER, "%s", temp_result);

		/* detect erroneous result. can be non-numeric */
		if (!validate_result(poller_items[i].result)) {
			report_invalid_script_result(poller_items, i, context);

			SET_UNDEFINED(poller_items[i].result);
		}
	}

}

static void poll_script_item(spine_spine_host_t *host, target_t *poller_items, int i, const legacy_result_context_t *context) {
	char *poll_result = NULL;
	/* Reject empty script commands that could cause unexpected behavior */
	if (strlen(poller_items[i].arg1) == 0) {
		SPINE_LOG(("WARNING: Device[%i] HT[%i] DS[%i] empty script command, skipping",
			context->host_id, context->spine_host_thread, poller_items[i].local_data_id));
		SET_UNDEFINED(poller_items[i].result);
		return;
	}

	poll_result = exec_poll(host, poller_items[i].arg1, poller_items[i].local_data_id, "DS");

	/* process the result */
	normalize_script_result(poller_items, i, poll_result, context);
	SPINE_FREE(poll_result);

	(*context->thread_end) = get_time_as_double();

	if (is_debug_device(context->host_id)) {
		SPINE_LOG(("Device[%i] HT[%i] DS[%i] TT[%.2f] SCRIPT: %s, output: %s", context->host_id, context->spine_host_thread, poller_items[i].local_data_id, (float) (((*context->thread_end) - (*context->thread_start)) * 1000), poller_items[i].arg1, poller_items[i].result));
	} else {
		SPINE_LOG_MEDIUM(("Device[%i] HT[%i] DS[%i] TT[%.2f] SCRIPT: %s, output: %s", context->host_id, context->spine_host_thread, poller_items[i].local_data_id, (float) (((*context->thread_end) - (*context->thread_start)) * 1000), poller_items[i].arg1, poller_items[i].result));
	}

	kill_legacy_scalar_spike(&poller_items[i], context);
}

static void poll_php_item(spine_spine_host_t *host, target_t *poller_items, int i, const legacy_result_context_t *context) {
	char *poll_result = NULL;
	int php_process;
	/* Reject empty script commands that could cause unexpected behavior */
	if (strlen(poller_items[i].arg1) == 0) {
		SPINE_LOG(("WARNING: Device[%i] HT[%i] DS[%i] empty script server command, skipping",
			context->host_id, context->spine_host_thread, poller_items[i].local_data_id));
		SET_UNDEFINED(poller_items[i].result);
		return;
	}

	php_process = php_get_process();

	poll_result = php_cmd(poller_items[i].arg1, php_process);

	/* process the output */
	normalize_script_result(poller_items, i, poll_result, context);
	SPINE_FREE(poll_result);

	(*context->thread_end) = get_time_as_double();

	if (is_debug_device(context->host_id)) {
		SPINE_LOG(("Device[%i] HT[%i] DS[%i] TT[%.2f] SS[%i] SERVER: %s, output: %s", context->host_id, context->spine_host_thread, poller_items[i].local_data_id, (float) (((*context->thread_end) - (*context->thread_start)) * 1000), php_process, poller_items[i].arg1, poller_items[i].result));
	} else {
		SPINE_LOG_MEDIUM(("Device[%i] HT[%i] DS[%i] TT[%.2f] SS[%i] SERVER: %s, output: %s", context->host_id, context->spine_host_thread, poller_items[i].local_data_id, (float) (((*context->thread_end) - (*context->thread_start)) * 1000), php_process, poller_items[i].arg1, poller_items[i].result));
	}

	kill_legacy_scalar_spike(&poller_items[i], context);
}

typedef struct {
	int  last_snmp_version;
	int  last_snmp_port;
	char last_snmp_community[50];
	char last_snmp_username[50];
	char last_snmp_password[50];
	char last_snmp_auth_protocol[16];
	char last_snmp_priv_passphrase[200];
	char last_snmp_priv_protocol[16];
	char last_snmp_context[65];
	char last_snmp_engine_id[30];
} legacy_snmp_profile_t;

typedef struct {
	int *k;
	int *num_oids;
	legacy_snmp_profile_t *profile;
	const legacy_result_context_t *results;
} legacy_snmp_batch_t;

static void capture_snmp_item_profile(legacy_snmp_profile_t *profile, const target_t *item) {
	profile->last_snmp_port = item->snmp_port;
	profile->last_snmp_version = item->snmp_version;

	STRNCOPY(profile->last_snmp_community,       item->snmp_community);
	STRNCOPY(profile->last_snmp_username,        item->snmp_username);
	STRNCOPY(profile->last_snmp_password,        item->snmp_password);
	STRNCOPY(profile->last_snmp_auth_protocol,   item->snmp_auth_protocol);
	STRNCOPY(profile->last_snmp_priv_passphrase, item->snmp_priv_passphrase);
	STRNCOPY(profile->last_snmp_priv_protocol,   item->snmp_priv_protocol);
	STRNCOPY(profile->last_snmp_context,         item->snmp_context);
	STRNCOPY(profile->last_snmp_engine_id,       item->snmp_engine_id);
}

static void poll_snmp_item(spine_spine_host_t *host, target_t *poller_items, snmp_oids_t *snmp_oids,
	int i, const legacy_snmp_batch_t *state) {
	int j;
	/* initialize or reinitialize snmp as required */
	if ((*state->k) == 0) {
		capture_snmp_item_profile(state->profile, &poller_items[i]);

		host->snmp_session = snmp_host_init(&(spine_snmp_profile_t){
			.host_id = host->id,
			.hostname = poller_items[i].hostname,
			.snmp_version = poller_items[i].snmp_version,
			.snmp_community = poller_items[i].snmp_community,
			.snmp_username = poller_items[i].snmp_username,
			.snmp_password = poller_items[i].snmp_password,
			.snmp_auth_protocol = poller_items[i].snmp_auth_protocol,
			.snmp_priv_passphrase = poller_items[i].snmp_priv_passphrase,
			.snmp_priv_protocol = poller_items[i].snmp_priv_protocol,
			.snmp_context = poller_items[i].snmp_context,
			.snmp_engine_id = poller_items[i].snmp_engine_id,
			.snmp_port = poller_items[i].snmp_port,
			.snmp_timeout = poller_items[i].snmp_timeout,
		});

		(*state->k)++;
	}

	/* catch snmp initialization issues */
	if (host->snmp_session == NULL) {
		host->ignore_host = TRUE;
		return;
	}

	/* some snmp data changed from poller item to poller item.  therefore, poll host and store data */
	if ((state->profile->last_snmp_port != poller_items[i].snmp_port) ||
		(state->profile->last_snmp_version != poller_items[i].snmp_version) ||
		(poller_items[i].snmp_version < 3 &&
		(!STRMATCH(state->profile->last_snmp_community, poller_items[i].snmp_community))) ||
		(poller_items[i].snmp_version > 2 &&
		((!STRMATCH(state->profile->last_snmp_username, poller_items[i].snmp_username)) ||
		(!STRMATCH(state->profile->last_snmp_password, poller_items[i].snmp_password)) ||
		(!STRMATCH(state->profile->last_snmp_auth_protocol, poller_items[i].snmp_auth_protocol)) ||
		(!STRMATCH(state->profile->last_snmp_priv_passphrase, poller_items[i].snmp_priv_passphrase)) ||
		(!STRMATCH(state->profile->last_snmp_priv_protocol, poller_items[i].snmp_priv_protocol)) ||
		(!STRMATCH(state->profile->last_snmp_context, poller_items[i].snmp_context)) ||
		(!STRMATCH(state->profile->last_snmp_engine_id, poller_items[i].snmp_engine_id))))) {

		if ((*state->num_oids) > 0) {
			snmp_get_multi(host, poller_items, snmp_oids, (*state->num_oids));

			for (j = 0; j < (*state->num_oids); j++) {
				consume_profile_switch_result(host, poller_items, snmp_oids, j, state->results);
			}

			/* reset num_snmps */
			(*state->num_oids) = 0;

			/* initialize all the memory to insure we don't get issues */
			memset(snmp_oids, 0, sizeof(snmp_oids_t)*host->max_oids);
		}

		SNMP_FREE(host->snmp_session);

		host->snmp_session = snmp_host_init(&(spine_snmp_profile_t){
			.host_id = host->id,
			.hostname = poller_items[i].hostname,
			.snmp_version = poller_items[i].snmp_version,
			.snmp_community = poller_items[i].snmp_community,
			.snmp_username = poller_items[i].snmp_username,
			.snmp_password = poller_items[i].snmp_password,
			.snmp_auth_protocol = poller_items[i].snmp_auth_protocol,
			.snmp_priv_passphrase = poller_items[i].snmp_priv_passphrase,
			.snmp_priv_protocol = poller_items[i].snmp_priv_protocol,
			.snmp_context = poller_items[i].snmp_context,
			.snmp_engine_id = poller_items[i].snmp_engine_id,
			.snmp_port = poller_items[i].snmp_port,
			.snmp_timeout = poller_items[i].snmp_timeout,
		});

		capture_snmp_item_profile(state->profile, &poller_items[i]);
	}

	if ((*state->num_oids) >= host->max_oids) {
		snmp_get_multi(host, poller_items, snmp_oids, (*state->num_oids));

		for (j = 0; j < (*state->num_oids); j++) {
			consume_full_batch_result(host, poller_items, snmp_oids, j, state->results);
		}

		/* reset num_snmps */
		(*state->num_oids) = 0;

		/* initialize all the memory to insure we don't get issues */
		memset(snmp_oids, 0, sizeof(snmp_oids_t)*host->max_oids);
	}

	snprintf(snmp_oids[(*state->num_oids)].oid, sizeof(snmp_oids[(*state->num_oids)].oid), "%s", poller_items[i].arg1);
	snmp_oids[(*state->num_oids)].array_position = i;
	(*state->num_oids)++;
}

/* The legacy direct writer retains caller-owned output buffers until the
 * existing cleanup phase; it does not own or release poller_items. */
typedef struct {
	const legacy_queries_t *queries;
	target_t *poller_items;
	int rows_processed;
	const char *spine_host_time;
	int host_id;
	int spine_host_thread;
	MYSQL *local;
	MYSQL *remote;
	char **query3;
	char **query12;
} legacy_output_t;

static void flush_legacy_output(const legacy_output_t *writer, MYSQL *mysqlt, int mode, size_t buf_length, int reset) {
	/* append the suffix */
	append_output_query((*writer->query3), buf_length, writer->queries->posuffix, writer->queries->posuffix_len);

	/* insert the record */
	db_insert(mysqlt, mode, (*writer->query3));

	if (reset) {
		/* re-initialize the query buffer */
		memset((*writer->query3), 0, buf_length);

		append_output_query((*writer->query3), buf_length, writer->queries->query8, writer->queries->query8_len);
	}

	/* insert the record for boost */
	if (set.boost_redirect && set.boost_enabled) {
		/* append the suffix */
		append_output_query((*writer->query12), buf_length, writer->queries->posuffix, writer->queries->posuffix_len);

		db_insert(mysqlt, mode, (*writer->query12));

		if (reset) {
			memset((*writer->query12), 0, buf_length);

			append_output_query((*writer->query12), buf_length, writer->queries->query11, writer->queries->query11_len);
		}
	}
}

static void write_legacy_output(const legacy_output_t *writer) {
	char result_string[(RESULTS_BUFFER * 2) + DBL_BUFSIZE + SMALL_BUFSIZE];
	int result_length;
	size_t out_buffer;
	size_t buf_length;
	int new_buffer = TRUE;
	int i;
	MYSQL mysqlt;
	buf_length = spine_output_buffer_size(MAX_MYSQL_BUF_SIZE, sizeof(result_string),
		(size_t)(writer->queries->query8_len > writer->queries->query11_len ? writer->queries->query8_len : writer->queries->query11_len), (size_t)writer->queries->posuffix_len);
	if (buf_length == 0) {
		set.exit_code = EXIT_FAILURE;
		die("ERROR: Poller output query buffer size overflow");
	}

	/* insert the query results into the database */
	if (!((*writer->query3) = (char *)malloc(buf_length))) {
		die("ERROR: Fatal malloc error: poller.c query3 output buffer!");
	}

	/* set zeros */
	memset((*writer->query3), 0, buf_length);

	/* append data */
	append_output_query((*writer->query3), buf_length, writer->queries->query8, writer->queries->query8_len);

	out_buffer = strlen(*writer->query3);

	if (set.boost_redirect && set.boost_enabled) {
		/* insert the query results into the database */
		if (!((*writer->query12) = (char *)malloc(buf_length))) {
			die("ERROR: Fatal malloc error: poller.c query12 boost output buffer!");
		}

		/* set zeros */
		memset((*writer->query12), 0, buf_length);

		/* append data */
		append_output_query((*writer->query12), buf_length, writer->queries->query11, writer->queries->query11_len);
	}

	int mode;
	if (set.poller_id > 1 && set.mode == REMOTE_ONLINE) {
		SPINE_LOG_DEBUG(("DEBUG: Setting up writes to remote database"));
		mysqlt = *writer->remote;
		mode   = REMOTE;
	} else {
		SPINE_LOG_DEBUG(("DEBUG: Setting up writes to local database"));
		mysqlt = *writer->local;
		mode   = LOCAL;
	}

	i = 0;
	while (i < writer->rows_processed) {
		/* Escaping can double each source byte, plus the terminator. */
		char escaped_result[(RESULTS_BUFFER * 2) + 1];
		char escaped_rrd_name[DBL_BUFSIZE];

		db_escape(&mysqlt, escaped_result, sizeof(escaped_result), writer->poller_items[i].result);
		db_escape(&mysqlt, escaped_rrd_name, sizeof(escaped_rrd_name), writer->poller_items[i].rrd_name);

		if (!format_poller_output_row(result_string, sizeof(result_string),
			writer->poller_items[i].local_data_id,
			escaped_rrd_name,
			writer->spine_host_time,
			escaped_result)) {
			SPINE_LOG(("Device[%i] HT[%i] ERROR: Poller output for DS[%i] "
				"exceeds the configured result buffer and was skipped",
				writer->host_id, writer->spine_host_thread, writer->poller_items[i].local_data_id));
			i++;
			continue;
		}

		result_length = strlen(result_string);

		/* if the next element to the buffer will overflow it, write to the database */
		if (spine_output_buffer_needs_flush(out_buffer, (size_t)result_length, MAX_MYSQL_BUF_SIZE)) {
			flush_legacy_output(writer, &mysqlt, mode, buf_length, TRUE);

			/* reset the output buffer length */
			out_buffer = strlen(*writer->query3);

			/* set binary, let the system know we are a new buffer */
			new_buffer = TRUE;
		}

		/* if this is our first pass, or we just outputted to the database, need to change the delimiter */
		if (new_buffer) {
			result_string[0] = ' ';
		} else {
			result_string[0] = ',';
		}

		append_output_query((*writer->query3), buf_length, result_string, result_length);

		if (set.boost_redirect && set.boost_enabled) {
			append_output_query((*writer->query12), buf_length, result_string, result_length);
		}

		out_buffer = out_buffer + strlen(result_string);
		new_buffer = FALSE;
		i++;
	}

	/* perform the last insert if there is data to process */
	if (out_buffer > strlen(writer->queries->query8)) {
		flush_legacy_output(writer, &mysqlt, mode, buf_length, FALSE);
	}
}

static void initialize_legacy_item(target_t *item) {
	/* initialize monitored object */
	item->target_id                = 0;
	item->action                   = -1;
	item->hostname[0]              = '\0';
	item->snmp_community[0]        = '\0';
	item->snmp_version             = 1;
	item->snmp_username[0]         = '\0';
	item->snmp_password[0]         = '\0';
	item->snmp_auth_protocol[0]    = '\0';
	item->snmp_priv_passphrase[0]  = '\0';
	item->snmp_priv_protocol[0]    = '\0';
	item->snmp_context[0]          = '\0';
	item->snmp_engine_id[0]        = '\0';
	item->snmp_port                = 161;
	item->snmp_timeout             = 500;
	item->rrd_name[0]              = '\0';
	item->rrd_path[0]              = '\0';
	item->arg1[0]                  = '\0';
	item->arg2[0]                  = '\0';
	item->arg3[0]                  = '\0';
	item->local_data_id            = 0;
	item->rrd_num                  = 0;
	item->output_regex[0]          = '\0';
}

static void load_legacy_item_source(target_t *item, MYSQL_ROW row) {
	if (row[0] != NULL)  item->action = atoi(row[0]);

	if (row[1] != NULL)  snprintf(item->hostname, sizeof(item->hostname), "%s", row[1]);
	if (row[2] != NULL)  snprintf(item->snmp_community, sizeof(item->snmp_community), "%s", row[2]);

	if (row[3] != NULL)  item->snmp_version = atoi(row[3]);

	if (row[4] != NULL)  snprintf(item->snmp_username, sizeof(item->snmp_username), "%s", row[4]);
	if (row[5] != NULL)  snprintf(item->snmp_password, sizeof(item->snmp_password), "%s", row[5]);
}

static void load_legacy_item_output(target_t *item, MYSQL_ROW row) {
	if (row[6]  != NULL) snprintf(item->rrd_name,      sizeof(item->rrd_name),      "%s", row[6]);
	if (row[7]  != NULL) snprintf(item->rrd_path,      sizeof(item->rrd_path),      "%s", row[7]);
	if (row[8]  != NULL) snprintf(item->arg1,          sizeof(item->arg1),          "%s", row[8]);
	if (row[9]  != NULL) snprintf(item->arg2,          sizeof(item->arg2),          "%s", row[9]);
	if (row[10] != NULL) snprintf(item->arg3,          sizeof(item->arg3),          "%s", row[10]);

	if (row[11] != NULL) item->local_data_id = atoi(row[11]);

	if (row[12] != NULL) item->rrd_num       = atoi(row[12]);
	if (row[13] != NULL) item->snmp_port     = atoi(row[13]);
	if (row[14] != NULL) item->snmp_timeout  = atoi(row[14]);
}

static void load_legacy_item_security(target_t *item, MYSQL_ROW row) {
	if (row[15] != NULL)  snprintf(item->snmp_auth_protocol,
		sizeof(item->snmp_auth_protocol), "%s", row[15]);
	if (row[16] != NULL)  snprintf(item->snmp_priv_passphrase,
		sizeof(item->snmp_priv_passphrase), "%s", row[16]);
	if (row[17] != NULL)  snprintf(item->snmp_priv_protocol,
		sizeof(item->snmp_priv_protocol), "%s", row[17]);
	if (row[18] != NULL)  snprintf(item->snmp_context,
		sizeof(item->snmp_context), "%s", row[18]);
	if (row[19] != NULL)  snprintf(item->snmp_engine_id,
		sizeof(item->snmp_engine_id), "%s", row[19]);
}

static void load_legacy_item(target_t *item, MYSQL_ROW row) {
	initialize_legacy_item(item);
	load_legacy_item_source(item, row);
	load_legacy_item_output(item, row);
	load_legacy_item_security(item, row);
	if (set.has_output_regex && row[20] != NULL)
		snprintf(item->output_regex,
			sizeof(item->output_regex), "%s", row[20]);

	SET_UNDEFINED(item->result);
}

typedef struct {
	int host_id;
	int spine_host_thread;
	int host_data_ids;
	char *spine_host_time;
	int *host_errors;
	HostPollPipelineData *pipeline_data;
} legacy_poll_request_t;

typedef struct {
	pool_t *local_cnn;
	pool_t *remote_cnn;
	MYSQL mysql;
	MYSQL mysqlr;
	spine_spine_host_t *host;
	ping_t *ping;
	reindex_t *reindex;
} legacy_poll_resources_t;

typedef struct {
	char *error_string;
	int *buf_size;
	int *buf_errors;
	int errors;
	int spike_kill;
} legacy_poll_report_t;

/* Internal stack ownership: request fields are borrowed; resources and
 * reporting buffers keep the legacy acquire/release order. No public layout
 * or serialization contract uses this context. */
typedef struct {
	legacy_poll_request_t request;
	legacy_poll_resources_t resources;
	legacy_poll_report_t report;
	legacy_queries_t queries;
	double poll_time;
} legacy_poll_t;

static int acquire_legacy_poll_resources(legacy_poll_t *poll) {
	poll->report.error_string = calloc(1, DBL_BUFSIZE);
	poll->report.buf_size     = malloc(sizeof(int));
	poll->report.buf_errors   = malloc(sizeof(int));

	if (poll->report.error_string == NULL || poll->report.buf_size == NULL || poll->report.buf_errors == NULL) {
		set.exit_code = EXIT_FAILURE;
		die("ERROR: Failed to allocate polling error buffers");
	}

	*poll->report.buf_size     = 0;
	*poll->report.buf_errors   = 0;


	poll->resources.local_cnn = db_get_connection(LOCAL);
	if (poll->resources.local_cnn == NULL) {
		SPINE_LOG(("FATAL: Device[%i] HT[%i] Unable to acquire local DB connection", poll->request.host_id, poll->request.spine_host_thread));
		SPINE_FREE(poll->report.error_string);
		SPINE_FREE(poll->report.buf_size);
		SPINE_FREE(poll->report.buf_errors);
		return FALSE;
	}
	poll->resources.mysql = poll->resources.local_cnn->mysql;

	if (set.poller_id > 1 && set.mode == REMOTE_ONLINE) {
		poll->resources.remote_cnn = db_get_connection(REMOTE);
		if (poll->resources.remote_cnn == NULL) {
			SPINE_LOG(("FATAL: Device[%i] HT[%i] Unable to acquire remote DB connection", poll->request.host_id, poll->request.spine_host_thread));
			db_release_connection(LOCAL, poll->resources.local_cnn->id);
			SPINE_FREE(poll->report.error_string);
			SPINE_FREE(poll->report.buf_size);
			SPINE_FREE(poll->report.buf_errors);
			return FALSE;
		}
		poll->resources.mysqlr = poll->resources.remote_cnn->mysql;
	}

	/* allocate host and ping structures with appropriate values.
	 * On OOM, release DB connections and return rather than die(): a single
	 * poller thread failure must not take down the entire spine process. */
	if (!(poll->resources.host = (spine_spine_host_t *) malloc(sizeof(spine_spine_host_t)))) {
		SPINE_LOG(("ERROR: Device[%i] HT[%i] malloc failed for host struct", poll->request.host_id, poll->request.spine_host_thread));
		db_release_connection(LOCAL, poll->resources.local_cnn->id);
		if (set.poller_id > 1 && set.mode == REMOTE_ONLINE && poll->resources.remote_cnn != NULL) {
			db_release_connection(REMOTE, poll->resources.remote_cnn->id);
		}
		SPINE_FREE(poll->report.error_string);
		SPINE_FREE(poll->report.buf_size);
		SPINE_FREE(poll->report.buf_errors);
		return FALSE;
	}
	memset(poll->resources.host, 0, sizeof(spine_spine_host_t));

	if (!(poll->resources.ping = (ping_t *) malloc(sizeof(ping_t)))) {
		SPINE_LOG(("ERROR: Device[%i] HT[%i] malloc failed for ping struct", poll->request.host_id, poll->request.spine_host_thread));
		SPINE_FREE(poll->resources.host);
		db_release_connection(LOCAL, poll->resources.local_cnn->id);
		if (set.poller_id > 1 && set.mode == REMOTE_ONLINE && poll->resources.remote_cnn != NULL) {
			db_release_connection(REMOTE, poll->resources.remote_cnn->id);
		}
		SPINE_FREE(poll->report.error_string);
		SPINE_FREE(poll->report.buf_size);
		SPINE_FREE(poll->report.buf_errors);
		return FALSE;
	}
	memset(poll->resources.ping, 0, sizeof(ping_t));

	if (!(poll->resources.reindex = (reindex_t *) malloc(sizeof(reindex_t)))) {
		SPINE_LOG(("ERROR: Device[%i] HT[%i] malloc failed for reindex struct", poll->request.host_id, poll->request.spine_host_thread));
		SPINE_FREE(poll->resources.host);
		SPINE_FREE(poll->resources.ping);
		db_release_connection(LOCAL, poll->resources.local_cnn->id);
		if (set.poller_id > 1 && set.mode == REMOTE_ONLINE && poll->resources.remote_cnn != NULL) {
			db_release_connection(REMOTE, poll->resources.remote_cnn->id);
		}
		SPINE_FREE(poll->report.error_string);
		SPINE_FREE(poll->report.buf_size);
		SPINE_FREE(poll->report.buf_errors);
		return FALSE;
	}
	memset(poll->resources.reindex, 0, sizeof(reindex_t));

	return TRUE;
}

static void release_legacy_poll_connections(const legacy_poll_t *poll) {
	if (poll->resources.local_cnn != NULL) {
		db_release_connection(LOCAL, poll->resources.local_cnn->id);
	} else {
		SPINE_LOG(("WARNING: Device[%i] HT[%i] Trying to close uninitialized local connection.", poll->request.host_id, poll->request.spine_host_thread));
	}

	if (set.poller_id > 1 && set.mode == REMOTE_ONLINE) {
		if (poll->resources.remote_cnn != NULL) {
			db_release_connection(REMOTE, poll->resources.remote_cnn->id);
		} else {
			SPINE_LOG(("WARNING: Device[%i] HT[%i] Trying to close uninitialized remote connection.", poll->request.host_id, poll->request.spine_host_thread));
		}
	}
}

static void load_legacy_host_details(legacy_poll_t *poll, MYSQL_ROW row, MYSQL_RES *result, int *ignore_sysinfo) {
	initialize_legacy_host(poll->resources.host);

	load_legacy_host_snmp(poll->resources.host, row);
	load_legacy_host_availability(poll->resources.host, row);
	load_legacy_host_system(poll->resources.host, row, &poll->resources.mysql);

	/* correct max_oid bounds issues */
	if ((poll->resources.host->max_oids == 0) || (poll->resources.host->max_oids > 100)) {
		SPINE_LOG(("Device[%i] HT[%i] WARNING: Max OIDS is out of range with value of '%i'.  Resetting to default of 5", poll->request.host_id, poll->request.spine_host_thread, poll->resources.host->max_oids));
		poll->resources.host->max_oids = 5;
	}

	/* free the host result */
	db_free_result(result);

	if (((poll->resources.host->snmp_version >= 1) && (poll->resources.host->snmp_version <= 2) &&
		(strlen(poll->resources.host->snmp_community) > 0)) ||
		(poll->resources.host->snmp_version == 3)) {
		poll->resources.host->snmp_session = snmp_host_init(&(spine_snmp_profile_t){
			.host_id = poll->resources.host->id,
			.hostname = poll->resources.host->hostname,
			.snmp_version = poll->resources.host->snmp_version,
			.snmp_community = poll->resources.host->snmp_community,
			.snmp_username = poll->resources.host->snmp_username,
			.snmp_password = poll->resources.host->snmp_password,
			.snmp_auth_protocol = poll->resources.host->snmp_auth_protocol,
			.snmp_priv_passphrase = poll->resources.host->snmp_priv_passphrase,
			.snmp_priv_protocol = poll->resources.host->snmp_priv_protocol,
			.snmp_context = poll->resources.host->snmp_context,
			.snmp_engine_id = poll->resources.host->snmp_engine_id,
			.snmp_port = poll->resources.host->snmp_port,
			.snmp_timeout = poll->resources.host->snmp_timeout,
		});
	} else {
		poll->resources.host->snmp_session = NULL;
	}

	check_legacy_host_availability(poll->resources.host, poll->resources.ping, &poll->resources.mysql, poll->request.spine_host_thread, ignore_sysinfo);

	persist_legacy_host_status(poll->resources.host, &poll->resources.mysql, poll->request.spine_host_thread, *ignore_sysinfo);
}

static int load_legacy_poll_host(legacy_poll_t *poll) {
	MYSQL_ROW row;
	int ignore_sysinfo = TRUE;
	/* initialize the ping structure variables */
	snprintf(poll->resources.ping->ping_status,   50,            "down");
	snprintf(poll->resources.ping->ping_response, SMALL_BUFSIZE, "Ping not performed due to setting.");
	snprintf(poll->resources.ping->snmp_status,   50,            "down");
	snprintf(poll->resources.ping->snmp_response, SMALL_BUFSIZE, "SNMP not performed due to setting or ping result");

	/* if the host is a real host.  Note host_id=0 is not host based data source */
	if (poll->request.host_id) {
		MYSQL_RES *result;
		/* get data about this host */
		if ((result = db_query(&poll->resources.mysql, LOCAL, poll->queries.query2)) != 0) {
			int num_rows = mysql_num_rows(result);

			if (num_rows != 1) {
				db_free_result(result);

				release_legacy_poll_connections(poll);

				SPINE_FREE(poll->resources.host);
				SPINE_FREE(poll->resources.reindex);
				SPINE_FREE(poll->resources.ping);
				SPINE_FREE(poll->report.error_string);
				SPINE_FREE(poll->report.buf_size);
				SPINE_FREE(poll->report.buf_errors);

				mysql_thread_end();

				return FALSE;
			}

			/* fetch the result */
			row = mysql_fetch_row(result);

			if (row) {
				load_legacy_host_details(poll, row, result, &ignore_sysinfo);
			} else {
				SPINE_LOG(("Device[%i] HT[%i] ERROR: MySQL Returned a Null Device Result", poll->resources.host->id, poll->request.spine_host_thread));
				poll->resources.host->ignore_host = TRUE;
			}
		} else {
			poll->resources.host->ignore_host = TRUE;
		}
	} else {
		poll->resources.host->id           = 0;
		poll->resources.host->max_oids     = 1;
		poll->resources.host->snmp_session = NULL;
		poll->resources.host->ignore_host  = FALSE;
	}

	return TRUE;
}

static void cleanup_legacy_ping_only(legacy_poll_t *poll) {
	SPINE_FREE(poll->resources.host);
	SPINE_FREE(poll->resources.reindex);
	SPINE_FREE(poll->resources.ping);
	SPINE_FREE(poll->report.error_string);
	SPINE_FREE(poll->report.buf_size);
	SPINE_FREE(poll->report.buf_errors);

	release_legacy_poll_connections(poll);

	mysql_thread_end();

}

typedef struct {
	char sysUptime[BUFSIZE];
	int previous_assert_failure;
	int last_data_query_id;
} legacy_reindex_cache_t;

static void process_legacy_reindex_row(legacy_poll_t *poll, MYSQL_ROW row, legacy_reindex_cache_t *cache) {
	int assert_fail;
	int reindex_err;
	int perform_assert = TRUE;
	char *poll_result;

		assert_fail = FALSE;
		reindex_err = FALSE;

		/* initialize the reindex struction */
		poll->resources.reindex->data_query_id   = 0;
		poll->resources.reindex->action          = -1;
		poll->resources.reindex->op[0]           = '\0';
		poll->resources.reindex->assert_value[0] = '\0';
		poll->resources.reindex->arg1[0]         = '\0';

		if (row[0] != NULL) poll->resources.reindex->data_query_id = atoi(row[0]);
		if (row[1] != NULL) poll->resources.reindex->action        = atoi(row[1]);

		if (row[2] != NULL) snprintf(poll->resources.reindex->op, sizeof(poll->resources.reindex->op), "%s", row[2]);

		if (row[3] != NULL) snprintf(poll->resources.reindex->assert_value, sizeof(poll->resources.reindex->assert_value), "%s", row[3]);

		if (row[4] != NULL) snprintf(poll->resources.reindex->arg1, sizeof(poll->resources.reindex->arg1), "%s", row[4]);

		/* shortcut assertion checks if a data query reindex has already been queued */
		if ((cache->last_data_query_id == poll->resources.reindex->data_query_id) &&
			(!cache->previous_assert_failure)) {
			perform_assert = TRUE;
		} else if (cache->last_data_query_id != poll->resources.reindex->data_query_id) {
			cache->last_data_query_id = poll->resources.reindex->data_query_id;
			perform_assert = TRUE;
			cache->previous_assert_failure = FALSE;
		} else {
			perform_assert = FALSE;
		}

		poll_result = NULL;

		if (perform_assert) {
			poll_result = poll_reindex_source(poll->resources.host, poll->resources.reindex, poll->request.spine_host_thread, cache->sysUptime, &reindex_err);

			if (!reindex_err) {
				const reindex_assertion_t assertion = {
					.host_id = poll->request.host_id, .spine_host_thread = poll->request.spine_host_thread,
					.assert_fail = &assert_fail, .previous_assert_failure = &cache->previous_assert_failure,
					.errors = &poll->report.errors, .spike_kill = &poll->report.spike_kill,
					.local = &poll->resources.mysql, .remote = &poll->resources.mysqlr
				};
				evaluate_reindex_result(poll->resources.host, poll->resources.reindex, &assertion, poll_result);
				poll_result = NULL;
			}
		}
}

static void consume_legacy_reindex_cache(legacy_poll_t *poll, MYSQL_RES *result) {
	MYSQL_ROW row;
	int num_rows;
	legacy_reindex_cache_t cache;
	cache.previous_assert_failure = FALSE;
	cache.last_data_query_id = 0;

		num_rows = mysql_num_rows(result);

		if (num_rows > 0) {
			if (is_debug_device(poll->resources.host->id)) {
				SPINE_LOG(("Device[%i] HT[%i] DEBUG: RECACHE: Processing %i items in the auto reindex cache for '%s'", poll->resources.host->id, poll->request.spine_host_thread, num_rows, poll->resources.host->hostname));
			} else {
				SPINE_LOG_DEBUG(("Device[%i] HT[%i] DEBUG: RECACHE: Processing %i items in the auto reindex cache for '%s'", poll->resources.host->id, poll->request.spine_host_thread, num_rows, poll->resources.host->hostname));
			}

			// Cache uptime in case we need it again
			cache.sysUptime[0] = '\0';
			while ((row = mysql_fetch_row(result))) {
				process_legacy_reindex_row(poll, row, &cache);
			}
		} else {
			if (is_debug_device(poll->resources.host->id)) {
				SPINE_LOG(("Device[%i] HT[%i] Device has no information for recache.", poll->resources.host->id, poll->request.spine_host_thread));
			} else {
				SPINE_LOG_HIGH(("Device[%i] HT[%i] Device has no information for recache.", poll->resources.host->id, poll->request.spine_host_thread));
			}
		}

		/* free the host result */
		db_free_result(result);
}

static void process_legacy_reindex(legacy_poll_t *poll) {
	/* do the reindex check for this host if not script based */
	if ((!poll->resources.host->ignore_host) && (poll->request.host_id)) {
		MYSQL_RES *result;
		if ((result = db_query(&poll->resources.mysql, LOCAL, poll->queries.query4)) != 0) {
			consume_legacy_reindex_cache(poll, result);
		} else {
			SPINE_LOG(("Device[%i] HT[%i] ERROR: RECACHE Query Returned Null Result!", poll->resources.host->id, poll->request.spine_host_thread));
		}

		/* close the host snmp session, we will create again momentarily */
		SNMP_FREE(poll->resources.host->snmp_session);
	}

}

static MYSQL_RES *select_legacy_items(legacy_poll_t *poll, int *num_rows) {
	MYSQL_RES *result;
	/* calculate the number of poller items to poll this cycle */
	(*num_rows) = 0;
	if (set.poller_interval == 0) {
		/* get the poller items */
		if ((result = db_query(&poll->resources.mysql, LOCAL, poll->queries.query1)) != 0) {
			(*num_rows) = mysql_num_rows(result);
		} else {
			SPINE_LOG(("Device[%i] HT[%i] ERROR: Unable to Retrieve Rows due to Null Result!", poll->resources.host->id, poll->request.spine_host_thread));
		}
	} else {
		/* get the poller items */
		if ((result = db_query(&poll->resources.mysql, LOCAL, poll->queries.query5)) != 0) {
			(*num_rows) = mysql_num_rows(result);
		} else {
			SPINE_LOG(("Device[%i] HT[%i] ERROR: Unable to Retrieve Rows due to Null Result!", poll->resources.host->id, poll->request.spine_host_thread));
		}
	}

	return result;
}

static void dispatch_legacy_item(spine_spine_host_t *host, target_t *poller_items,
	snmp_oids_t *snmp_oids, int i, const legacy_snmp_batch_t *batch) {
	switch(poller_items[i].action) {
	case POLLER_ACTION_SNMP: /* raw SNMP poll */
		poll_snmp_item(host, poller_items, snmp_oids, i, batch);
		break;
	case POLLER_ACTION_SCRIPT: /* execute script file */
		poll_script_item(host, poller_items, i, batch->results);
		break;
	case POLLER_ACTION_PHP_SCRIPT_SERVER: /* execute script server */
		poll_php_item(host, poller_items, i, batch->results);
		break;
	default: /* unknown action, generate error */
		SPINE_LOG(("Device[%i] HT[%i] DS[%i] ERROR: Unknown Poller Action: %s", batch->results->host_id, batch->results->spine_host_thread, poller_items[i].local_data_id, poller_items[i].arg1));

		break;
	}
}

static void poll_legacy_items(legacy_poll_t *poll) {
	MYSQL_RES *result;
	MYSQL_ROW row;
	int num_rows;
	int rows_processed = 0;
	int i = 0;
	int k = 0;
	int num_oids = 0;
	double thread_start = 0;
	double thread_end = 0;
	target_t *poller_items = NULL;
	snmp_oids_t *snmp_oids = NULL;
	char *query3 = NULL;
	char *query12 = NULL;
	legacy_snmp_profile_t profile;
	profile.last_snmp_version = 0;
	profile.last_snmp_port = 0;
	result = select_legacy_items(poll, &num_rows);

	if (num_rows <= 0) {
		db_free_result(result);
		return;
	}

	/* retrieve each hosts polling items from poller cache and load into array */
	if (!(poller_items = (target_t *) calloc(num_rows, sizeof(target_t)))) {
		die("ERROR: Fatal calloc error: poller.c poller_items!");
	}

	i = 0;
	while ((row = mysql_fetch_row(result))) {
		load_legacy_item(&poller_items[i], row);
		i++;
	}

	/* free the mysql result */
	db_free_result(result);

	/* create an array for snmp oids */
	if (!(snmp_oids = (snmp_oids_t *) calloc(poll->resources.host->max_oids, sizeof(snmp_oids_t)))) {
		die("ERROR: Fatal calloc error: poller.c snmp_oids!");
	}

	/* initialize all the memory to insure we don't get issues */
	memset(snmp_oids, 0, sizeof(snmp_oids_t)*poll->resources.host->max_oids);

	const legacy_result_context_t result_context = {
		.host_id = poll->request.host_id, .spine_host_thread = poll->request.spine_host_thread,
		.error_string = poll->report.error_string, .buf_size = poll->report.buf_size, .buf_errors = poll->report.buf_errors,
		.errors = &poll->report.errors, .thread_start = &thread_start, .thread_end = &thread_end,
		.spike_kill = &poll->report.spike_kill
	};

	const legacy_snmp_batch_t snmp_batch = {
		.k = &k, .num_oids = &num_oids, .profile = &profile, .results = &result_context
	};

	/* log an informative message */
	if (is_debug_device(poll->request.host_id)) {
		SPINE_LOG(("Device[%i] HT[%i] NOTE: There are '%i' Polling Items for this Device", poll->request.host_id, poll->request.spine_host_thread, num_rows));
	} else {
		SPINE_LOG_MEDIUM(("Device[%i] HT[%i] NOTE: There are '%i' Polling Items for this Device", poll->request.host_id, poll->request.spine_host_thread, num_rows));
	}

	i = 0; k = 0;
	while ((i < num_rows) && (!poll->resources.host->ignore_host)) {
		thread_start = get_time_as_double();

		dispatch_legacy_item(poll->resources.host, poller_items, snmp_oids, i, &snmp_batch);

		i++;
		rows_processed++;
	}

	/* process last multi-get request if applicable */
	if (num_oids > 0) {
		snmp_get_multi(poll->resources.host, poller_items, snmp_oids, num_oids);

		for (int j = 0; j < num_oids; j++) {
			consume_final_batch_result(poll->resources.host, poller_items, snmp_oids, j, &result_context);
		}
	}

		if (poll->request.pipeline_data != NULL) {
			poll->request.pipeline_data->poller_items = poller_items;
			poll->request.pipeline_data->rows_processed = rows_processed;
			snprintf(poll->request.pipeline_data->query8, sizeof(poll->request.pipeline_data->query8), "%s", poll->queries.query8);
			snprintf(poll->request.pipeline_data->query11, sizeof(poll->request.pipeline_data->query11), "%s", poll->queries.query11);
			snprintf(poll->request.pipeline_data->posuffix, sizeof(poll->request.pipeline_data->posuffix), "%s", poll->queries.posuffix);
			poll->request.pipeline_data->query8_len = poll->queries.query8_len;
			poll->request.pipeline_data->query11_len = poll->queries.query11_len;
			poll->request.pipeline_data->posuffix_len = poll->queries.posuffix_len;
			poller_items = NULL;
		} else {
			const legacy_output_t writer = {
				.queries = &poll->queries, .poller_items = poller_items, .rows_processed = rows_processed,
				.spine_host_time = poll->request.spine_host_time,
				.host_id = poll->request.host_id, .spine_host_thread = poll->request.spine_host_thread,
				.local = &poll->resources.mysql, .remote = &poll->resources.mysqlr,
				.query3 = &query3, .query12 = &query12
			};
			write_legacy_output(&writer);
		}

	/* cleanup memory and prepare for function exit */
	SNMP_FREE(poll->resources.host->snmp_session);
	SPINE_FREE(query3);
	if (set.boost_redirect && set.boost_enabled) {
		SPINE_FREE(query12);
	}

	/* Zero per-device SNMP credentials before release so a late
	 * memory scan or a delayed core dump cannot read them back. */
	spine_scrub_target_secrets(poller_items, (int)num_rows);
	SPINE_FREE(poller_items);
	SPINE_FREE(snmp_oids);
}

static void finish_legacy_poll(legacy_poll_t *poll) {
	spine_scrub_host_secrets(poll->resources.host);
	SPINE_FREE(poll->resources.host);
	SPINE_FREE(poll->resources.reindex);
	SPINE_FREE(poll->resources.ping);

		/* record the polling time for the device */
		poll->poll_time = get_time_as_double() - poll->poll_time;
	if (is_debug_device(poll->request.host_id)) {
		SPINE_LOG(("Device[%i] HT[%i] Total Time: %0.2g Seconds", poll->request.host_id, poll->request.spine_host_thread, poll->poll_time));
	} else {
		SPINE_LOG_MEDIUM(("Device[%i] HT[%i] Total Time: %0.2g Seconds", poll->request.host_id, poll->request.spine_host_thread, poll->poll_time));
	}

		/* Pipeline stages now own host-state and host_errors persistence.
		 * Capture DS id rollups so persist stage can write them. */
		if (poll->request.pipeline_data != NULL) {
			snprintf(poll->request.pipeline_data->error_data_ids, sizeof(poll->request.pipeline_data->error_data_ids), "%s", poll->report.error_string);
		}

	release_legacy_poll_connections(poll);

	mysql_thread_end();

	if (is_debug_device(poll->request.host_id)) {
		SPINE_LOG(("Device[%i] HT[%i] DEBUG: HOST COMPLETE: About to Exit Device Polling Thread Function", poll->request.host_id, poll->request.spine_host_thread));
	} else {
		SPINE_LOG_DEBUG(("Device[%i] HT[%i] DEBUG: HOST COMPLETE: About to Exit Device Polling Thread Function", poll->request.host_id, poll->request.spine_host_thread));
	}

	if (set.spine_log_level == 1) {
		buffer_output_errors(poll->report.error_string, poll->report.buf_size, poll->report.buf_errors, poll->request.host_id, poll->request.spine_host_thread, 0, true);
	}

	SPINE_FREE(poll->report.error_string);
	SPINE_FREE(poll->report.buf_size);
	SPINE_FREE(poll->report.buf_errors);

	*poll->request.host_errors = poll->report.errors;

	SPINE_PROBE2(poll_done, poll->request.host_id, poll->report.errors);
}

/*! \fn void poll_host(int device_counter, int host_id, int spine_host_thread, int spine_host_threads, int host_data_ids, char *spine_host_time, int *host_errors, double spine_host_time_double)
 *  \brief core Spine function that polls a host
 *  \param host_id integer value for the host_id from the hosts table in Cacti
 *
 *  This function is core to Spine. It takes a host_id and polls it, first
 *  checking reachability and any required data-query reindexing.
 */
static void poll_host_legacy(int host_id, int spine_host_thread, int host_data_ids, char *spine_host_time, int *host_errors, HostPollPipelineData *pipeline_data) {
	legacy_poll_t owned;
	legacy_poll_t *poll = &owned;
	poll->request.host_id = host_id;
	poll->request.spine_host_thread = spine_host_thread;
	poll->request.host_data_ids = host_data_ids;
	poll->request.spine_host_time = spine_host_time;
	poll->request.host_errors = host_errors;
	poll->request.pipeline_data = pipeline_data;
	poll->resources.local_cnn = NULL;
	poll->resources.remote_cnn = NULL;
	poll->resources.host = NULL;
	poll->resources.ping = NULL;
	poll->resources.reindex = NULL;
	poll->report.errors = 0;
	poll->report.spike_kill = FALSE;
	poll->queries.query8_len = 0;
	poll->queries.query11_len = 0;
	poll->queries.posuffix_len = 0;

	SPINE_PROBE1(poll_start, host_id);
	poll->poll_time = get_time_as_double();
	if (!acquire_legacy_poll_resources(poll)) {
		return;
	}
	build_legacy_poll_queries(&poll->queries, host_id, host_data_ids, spine_host_thread);
	if (!load_legacy_poll_host(poll)) {
		return;
	}
	if (set.ping_only) {
		cleanup_legacy_ping_only(poll);
		return;
	}
	process_legacy_reindex(poll);
	poll_legacy_items(poll);
	finish_legacy_poll(poll);
}

/*! \fn void buffer_output_errors(local_data_id) {
 *  \brief buffers output errors and pushes those errors to standard
 *         output as required.
 *  \param char* buffer - pointer to the output buffer
 *  \param int device_id - the device id
 *  \param int thread id - the device thread
 *  \param int local_data_id - the local data id
 *  \param boolean flush - flush any part of buffer
 */
void buffer_output_errors(char *error_string, int *buf_size, int *buf_errors, int device_id, int thread_id, int local_data_id, bool flush) {
	int error_len;
	char tbuffer[SMALL_BUFSIZE];

	if (flush && *buf_errors > 0) {
		SPINE_LOG(("WARNING: Invalid Response(s), Errors[%i] Device[%i] Thread[%i] DS[%s]", *buf_errors, device_id, thread_id, error_string));
	} else if (!flush) {
		snprintf(tbuffer, SMALL_BUFSIZE, *buf_errors > 0 ? ", %i" : "%i", local_data_id);
		error_len = strlen(tbuffer);
		if (*buf_size + error_len >= DBL_BUFSIZE) {
			SPINE_LOG(("WARNING: Invalid Response(s), Errors[%i] Device[%i] Thread[%i] DS[%s]", *buf_errors, device_id, thread_id, error_string));
			*buf_errors  = 1;
			*buf_size = snprintf(error_string, DBL_BUFSIZE, "%i", local_data_id);
		} else {
			(*buf_errors)++;
			snprintf(error_string + *buf_size, DBL_BUFSIZE - *buf_size, "%s", tbuffer);
			*buf_size += error_len;
		}
	}
}

/*! \fn int is_multipart_output(char *result)
 *  \brief validates the output syntax is a valid name value pair syntax
 *  \param result the value to be checked for legality
 *
 *	This function will poll a specific host using the script pointed to by
 *  the command variable.
 *
 *  \return TRUE if the result is valid, otherwise FALSE.
 *
 */
int is_multipart_output(char *result) {
	int space_cnt = 0;
	int delim_cnt = 0;
	int i;

	/* check the easy cases first */
	if (result) {
		/* it must have delimiters */
		if ((strstr(result, ":")) || (strstr(result, "!"))) {
			if (!strstr(result, " ")) {
				return TRUE;
			} else {
				const int len = strlen(result);

				for (i=0; i<len; i++) {
					if ((result[i] == ':') || (result[i] == '!')) {
						delim_cnt = delim_cnt + 1;
					} else if (result[i] == ' ') {
						space_cnt = space_cnt + 1;
					}
				}

				if (space_cnt+1 == delim_cnt) {
					return TRUE;
				} else {
					return FALSE;
				}
			}
		}
	}

	return FALSE;
}

void get_system_information(spine_spine_host_t *host, MYSQL *mysql, int system)  {
	char *poll_result;

	SPINE_LOG_MEDIUM(("Device[%d] Checking for System Information Update", host->id));

	if (set.mibs || system) {
		if (is_debug_device(host->id)) {
			SPINE_LOG(("Device[%d] Updating Full System Information Table", host->id));
		} else {
			SPINE_LOG_MEDIUM(("Device[%d] Updating Full System Information Table", host->id));
		}

		SPINE_LOG_DEVDBG(("DEVDBG: Device[%d] poll_result = snmp_get_allow_fail(host, '.1.3.6.1.2.1.1.1.0');", host->id));
		poll_result = snmp_get_allow_fail(host, ".1.3.6.1.2.1.1.1.0");
		SPINE_LOG_DEVDBG(("DEVDBG: Device[%d] poll_result = snmp_get_allow_fail(host, '.1.3.6.1.2.1.1.1.0'); [complete]", host->id));

		if (poll_result) {
			db_escape(mysql, host->snmp_sysDescr, sizeof(host->snmp_sysDescr), poll_result);
			SPINE_FREE(poll_result);
		}

		SPINE_LOG_DEVDBG(("DEVDBG: Device[%d] poll_result = snmp_get_allow_fail(host, '.1.3.6.1.2.1.1.2.0');", host->id));
		poll_result = snmp_get_allow_fail(host, ".1.3.6.1.2.1.1.2.0");
		SPINE_LOG_DEVDBG(("DEVDBG: Device[%d] poll_result = snmp_get_allow_fail(host, '.1.3.6.1.2.1.1.2.0'); [complete]", host->id));

		if (poll_result) {
			db_escape(mysql, host->snmp_sysObjectID, sizeof(host->snmp_sysObjectID), poll_result);
			SPINE_FREE(poll_result);
		}

		// Get the legacy system uptime instance first
		SPINE_LOG_DEVDBG(("DEVDBG: Device[%d] poll_result = snmp_get_allow_fail(host, '.1.3.6.1.2.1.1.3.0');", host->id));
		poll_result = snmp_get_allow_fail(host, ".1.3.6.1.2.1.1.3.0");
		SPINE_LOG_DEVDBG(("DEVDBG: Device[%d] poll_result = snmp_get_allow_fail(host, '.1.3.6.1.2.1.1.3.0'); [complete]", host->id));

		if (poll_result && is_numeric(poll_result)) {
			host->snmp_sysUpTimeInstance = atoll(poll_result);
			SPINE_FREE(poll_result);

			// Attempt to get the more modern version
			SPINE_LOG_DEVDBG(("DEVDBG: Device[%d] poll_result = snmp_get_allow_fail(host, '.1.3.6.1.6.3.10.2.1.3.0');", host->id));
			poll_result = snmp_get_allow_fail(host, ".1.3.6.1.6.3.10.2.1.3.0");
			SPINE_LOG_DEVDBG(("DEVDGB: Device[%d] poll_result = snmp_get_allow_fail(host, '.1.3.6.1.6.3.10.2.1.3.0'); [complete]", host->id));

			if (poll_result && is_numeric(poll_result)) {
				host->snmp_sysUpTimeInstance = atoll(poll_result) * 100;
				snprintf(poll_result, BUFSIZE, "%llu", host->snmp_sysUpTimeInstance);
			}

			SPINE_FREE(poll_result);
		}

		SPINE_LOG_DEVDBG(("DEVDBG: Device [%d] poll_result = snmp_get_allow_fail(host, '.1.3.6.1.2.1.1.4.0');", host->id));
		poll_result = snmp_get_allow_fail(host, ".1.3.6.1.2.1.1.4.0");
		SPINE_LOG_DEVDBG(("DEVDBG: Device [%d] poll_result = snmp_get_allow_fail(host, '.1.3.6.1.2.1.1.4.0'); [complete]", host->id));

		if (poll_result) {
			db_escape(mysql, host->snmp_sysContact, sizeof(host->snmp_sysContact), poll_result);
			SPINE_FREE(poll_result);
		}

		SPINE_LOG_DEVDBG(("DEVDBG: Device [%d] poll_result = snmp_get_allow_fail(host, '.1.3.6.1.2.1.1.5.0');", host->id));
		poll_result = snmp_get_allow_fail(host, ".1.3.6.1.2.1.1.5.0");
		SPINE_LOG_DEVDBG(("DEVDBG: Device [%d] poll_result = snmp_get_allow_fail(host, '.1.3.6.1.2.1.1.5.0'); [complete]", host->id));

		if (poll_result) {
			db_escape(mysql, host->snmp_sysName, sizeof(host->snmp_sysName), poll_result);
			SPINE_FREE(poll_result);
		}

		SPINE_LOG_DEVDBG(("DEVDBG: Device [%d] poll_result = snmp_get_allow_fail(host, '.1.3.6.1.2.1.1.6.0');", host->id));
		poll_result = snmp_get_allow_fail(host, ".1.3.6.1.2.1.1.6.0");
		SPINE_LOG_DEVDBG(("DEVDBG: Device [%d] poll_result = snmp_get_allow_fail(host, '.1.3.6.1.2.1.1.6.0'); [complete]", host->id));

		if (poll_result) {
			db_escape(mysql, host->snmp_sysLocation, sizeof(host->snmp_sysLocation), poll_result);
			SPINE_FREE(poll_result);
		}
	} else {
		if (is_debug_device(host->id)) {
			SPINE_LOG(("Device[%d] Updating Short System Information Table", host->id));
		} else {
			SPINE_LOG_MEDIUM(("Device[%d] Updating Short System Information Table", host->id));
		}

		// Get the legacy system uptime instance first
		SPINE_LOG_DEVDBG(("DEVDBG: Device[%d] poll_result = snmp_get(host, '.1.3.6.1.2.1.1.3.0');", host->id));
		poll_result = snmp_get(host, ".1.3.6.1.2.1.1.3.0");
		SPINE_LOG_DEVDBG(("DEVDGB: Device[%d] poll_result = snmp_get(host, '.1.3.6.1.2.1.1.3.0'); [complete]", host->id));

		if (poll_result && is_numeric(poll_result)) {
			host->snmp_sysUpTimeInstance = atoll(poll_result);
			SPINE_FREE(poll_result);

			// Attempt to get the more modern version
			SPINE_LOG_DEVDBG(("DEVDBG: Device[%d] poll_result = snmp_get(host, '.1.3.6.1.6.3.10.2.1.3.0');", host->id));
			poll_result = snmp_get(host, ".1.3.6.1.6.3.10.2.1.3.0");
			SPINE_LOG_DEVDBG(("DEVDGB: Device[%d] poll_result = snmp_get(host, '.1.3.6.1.6.3.10.2.1.3.0'); [complete]", host->id));

			if (poll_result && is_numeric(poll_result)) {
				host->snmp_sysUpTimeInstance = atoll(poll_result) * 100;
				snprintf(poll_result, BUFSIZE, "%llu", host->snmp_sysUpTimeInstance);
			}

			SPINE_FREE(poll_result);
		}
	}
}

/*! \fn int validate_result(char *result)
 *  \brief validates the output from the polling action is valid
 *  \param result the value to be checked for legality
 *
 *	This function will poll a specific host using the script pointed to by
 *  the command variable.
 *
 *  \return TRUE if the result is valid, otherwise FALSE.
 *
 */
int validate_result(char *result) {
	/* check the easy cases first */
	if (result) {
		if (is_numeric(result)) {
			return TRUE;
		} else {
			if (is_multipart_output(trim(result))) {
				return TRUE;
			} else {
				return FALSE;
			}
		}
	}

	return FALSE;
}

/*! \fn char *exec_poll(spine_spine_host_t *current_host, char *command, int id, const char *type)
 *  \brief polls a host using a script
 *  \param current_host a pointer to the current host structure
 *  \param command the command to be executed
 *  \param id either the local_data_id or the data_query_id
 *
 *	This function will poll a specific host using the script pointed to by
 *  the command variable.
 *
 *  \return a pointer to a character buffer containing the result.
 *
 */
/* WARNING: command is passed to /bin/sh -c (via nft_popen) without shell escaping.
 * The caller MUST ensure command originates from a trusted source
 * (the Cacti database). Do not pass user-controlled input directly. */
char *exec_poll(spine_spine_host_t *current_host, char *command, int id, const char *type) {
	int cmd_fd;
	int pid;

	int bytes_read;
	double begin_time = 0;
	double end_time = 0;
	double script_timeout;
	double remaining_usec = 0;
	struct timeval timeout;
	char *proc_command;
	char *result_string;

	proc_command = command;

	if (!(result_string = (char *) malloc(RESULTS_BUFFER))) {
		die("ERROR: Fatal malloc error: poller.c exec_poll!");
	}

	/* set zeros */
	memset(result_string, 0, RESULTS_BUFFER);

	/* set script timeout as double */
	script_timeout = set.script_timeout;

	/* establish timeout of 25 seconds for pipe response */
	timeout.tv_sec = set.script_timeout;
	timeout.tv_usec = 0;

	/* don't run too many scripts, operating systems do not like that. */
	int retries = 0;
	int sem_err = 0;
	int needs_cleanup = 0;

	/* used for checking executable status */
	char executable[BUFSIZE];
	char *saveptr = NULL;

	pthread_cleanup_push(child_cleanup_script, NULL);

	// use the script server timeout value, allow for 50% leeway
	while (++retries < (set.script_timeout * 15)) {
		sem_err = spine_sem_trywait(&available_scripts);
		if (sem_err == 0) {
			break;
		} else {
			int sem_errno = errno;

			if (sem_errno == EAGAIN || sem_errno == EWOULDBLOCK) {
				if (is_debug_device(current_host->id)) {
					SPINE_LOG(("Device[%i] DEBUG: Pausing as unable to obtain a script execution lock", current_host->id));
				} else {
					SPINE_LOG_DEVDBG(("Device[%i] DEBUG: Pausing as unable to obtain a script execution lock", current_host->id));
				}
			} else {
				if (is_debug_device(current_host->id)) {
					SPINE_LOG(("Device[%i] DEBUG: Pausing as error %d whilst obtaining a script execution lock", current_host->id, sem_errno));
				} else {
					SPINE_LOG_DEVDBG(("Device[%i] DEBUG: Pausing as error %d whilst obtaining a script execution lock", current_host->id, sem_errno));
				}
			}
		}
		spine_platform_sleep_us(10000);
	}

	if (sem_err) {
		SPINE_LOG(("ERROR: Device[%i]: Failed to obtain a script execution lock within 30 seconds", current_host->id));
	} else {
		/* Mark for cleanup */
		needs_cleanup = 1;

		/* record start time */
		begin_time = get_time_as_double();

		/* peel the executable from the command */
		saveptr = proc_command;
		snprintf(executable, BUFSIZE, "%s", proc_command);
		strtok_r(executable, " ", &saveptr);

		/* cheesy little hack to add /usr/bin/ if its not included */
		if (strstr(executable, "/") == NULL) {
			saveptr = proc_command;
			snprintf(executable, BUFSIZE, "/usr/bin/%s", proc_command);
			strtok_r(executable, " ", &saveptr);
		}

		SPINE_LOG_DEBUG(("The executable is '%s' in \'%s\'", executable, proc_command));

		if (access(executable, X_OK | F_OK) != -1) {
			cmd_fd = nft_popen(proc_command, "r");
			if (is_debug_device(current_host->id)) {
				SPINE_LOG(("Device[%i] DEBUG: The NIFTY POPEN returned the following File Descriptor %i", current_host->id, cmd_fd));
			} else {
				SPINE_LOG_DEBUG(("Device[%i] DEBUG: The NIFTY POPEN returned the following File Descriptor %i", current_host->id, cmd_fd));
			}

			if (cmd_fd > 0) {
				retry:

				/* wait x seconds for pipe response */
				switch (spine_fd_wait_readable(cmd_fd, &timeout)) {
					case -1:
						switch (spine_fd_last_error()) {
							case EBADF:
								SPINE_LOG(("Device[%i] ERROR: One or more of the file descriptor sets specified a file descriptor that is not a valid open file descriptor.", current_host->id));
								SET_UNDEFINED(result_string);

								break;
							case EINTR:
								#ifndef SOLAR_THREAD
								/* take a moment */
								spine_platform_sleep_us(2000);
								#endif

								/* record end time */
								end_time = get_time_as_double();

								/* re-establish new timeout value */
								timeout.tv_sec  = rint(floor(script_timeout-(end_time-begin_time)));
								remaining_usec  = set.script_timeout - timeout.tv_sec - (end_time - begin_time);

								if (remaining_usec > 0) {
									timeout.tv_usec = rint(remaining_usec * 1000000);
								} else {
									timeout.tv_usec = 0;
								}
								timeout.tv_sec = rint(floor(script_timeout-(end_time-begin_time)));
								timeout.tv_usec = rint((script_timeout-(end_time-begin_time)-timeout.tv_sec)*1000000);

								if (timeout.tv_sec + timeout.tv_usec > 0) {
									goto retry;
								} else {
									SPINE_LOG(("WARNING: A script timed out while processing EINTR's."));
									SET_UNDEFINED(result_string);
								}
								break;
							case EINVAL:
								SPINE_LOG(("Device[%i] ERROR: Possible invalid timeout specified in pipe wait statement.", current_host->id));
								SET_UNDEFINED(result_string);
								break;
							default:
								SPINE_LOG(("Device[%i] ERROR: The script/command wait failed", current_host->id));
								SET_UNDEFINED(result_string);
								break;
						}

					break;
				case 0:
					SPINE_LOG_MEDIUM(("Device[%i] ERROR: The NIFTY POPEN timed out", current_host->id));

					pid = nft_pchild(cmd_fd);
					if (pid > 0) {
						kill(pid, SIGKILL);
					} else {
						SPINE_LOG(("Device[%i] ERROR: Unable to find the timed-out POPEN child", current_host->id));
					}

					SET_UNDEFINED(result_string);
					break;
				default:
					/* get only one line of output, we will ignore the rest */
					bytes_read = spine_fd_read(cmd_fd, result_string, RESULTS_BUFFER-1);
					if (bytes_read > 0) {
						result_string[bytes_read] = '\0';
					} else {
						char redacted_cmd[BUFSIZE];
						spine_redact_args(command, redacted_cmd, sizeof(redacted_cmd));
						if (STRIMATCH(type,"DS")) {
							SPINE_LOG(("Device[%i] DS[%i] ERROR: Empty result [%s]: '%s'", current_host->id, id, current_host->hostname, redacted_cmd));
						} else {
							SPINE_LOG(("Device[%i] DQ[%i] ERROR: Empty result [%s]: '%s'", current_host->id, id, current_host->hostname, redacted_cmd));
						}
						SET_UNDEFINED(result_string);
					}
				}

				/* close pipe */
				nft_pclose(cmd_fd);
			} else {
				char redacted_cmd[BUFSIZE];
				spine_redact_args(command, redacted_cmd, sizeof(redacted_cmd));
				SPINE_LOG(("Device[%i] ERROR: Problem executing POPEN [%s]: '%s'", current_host->id, current_host->hostname, redacted_cmd));
				SET_UNDEFINED(result_string);
			}
		} else {
			char redacted_cmd[BUFSIZE];
			spine_redact_args(command, redacted_cmd, sizeof(redacted_cmd));
			SPINE_LOG(("Device[%i] ERROR: Problem executing POPEN.  File '%s' does not exist or is not executable.", current_host->id, redacted_cmd));
			SET_UNDEFINED(result_string);
		}

	}

	/* reduce the active script count */
	pthread_cleanup_pop(needs_cleanup);

	return result_string;
}

#ifdef HAVE_LIBUV


static void poll_step(poll_context_t *ctx);

static void on_mux_complete(spine_task_t *task, int status, void *result) {
	(void)result;
	poll_context_t *ctx = (poll_context_t *)task->parent_ctx;
	ctx->mux_tasks_pending--;
	if (status != 0) ctx->host_errors++;
	if (ctx->mux_tasks_pending == 0) {
		ctx->state = POLL_STATE_SCRIPTS;
		spine_transition_state(ctx);
	}
}


static void on_handle_closed(uv_handle_t *handle) {
	poll_context_t *ctx = (poll_context_t *)handle->data;
	if (ctx->sessp) snmp_sess_close(ctx->sessp);
	spine_sem_post(&available_threads);
	free(ctx);
}

static void on_dns_complete(struct addrinfo *res, int status, void *data) {
	(void)res;
	poll_context_t *ctx = (poll_context_t *)data;
	ctx->state = (status == 0) ? POLL_STATE_PING : POLL_STATE_ERROR;
	poll_step(ctx);
}

static void __attribute__((unused)) on_ping_complete(const char *result, int status, void *data) {
	(void)result; (void)status;
	poll_context_t *ctx = (poll_context_t *)data;
	ctx->state = POLL_STATE_SNMP_SEND;
	poll_step(ctx);
}

static void __attribute__((unused)) on_snmp_complete(void *sessp, struct snmp_pdu *pdu, void *data) {
	(void)sessp; (void)pdu;
	poll_context_t *ctx = (poll_context_t *)data;
	ctx->state = POLL_STATE_SCRIPTS;
	poll_step(ctx);
}

static void __attribute__((unused)) on_php_complete(const char *result, void *data) {
	(void)result;
	poll_context_t *ctx = (poll_context_t *)data;
	ctx->state = POLL_STATE_FLUSH;
	poll_step(ctx);
}

static void __attribute__((unused)) on_exec_complete(const char *result, int exit_status, int term_signal, void *data) {
	(void)result; (void)exit_status; (void)term_signal;
	poll_context_t *ctx = (poll_context_t *)data;
	ctx->state = POLL_STATE_FLUSH;
	poll_step(ctx);
}

/* --- SRP: Discrete Stage Handlers --- */

static int stage_dns(poll_context_t *ctx) {
	if (ctx->host && ctx->host->hostname[0] != '\0') {
		return spine_async_dns_lookup_runtime(ctx->dns_runtime, ctx->host->hostname, on_dns_complete, ctx);
	}
	ctx->state = POLL_STATE_PING;
	return 1; 
}

static int stage_ping(poll_context_t *ctx) {
	ctx->state = POLL_STATE_SNMP_SEND;
	return 1;
}

static int stage_snmp(poll_context_t *ctx) {
	if (ctx->num_items > 0) {
		int items_per_task = 50;
		int i = 0;
		ctx->mux_tasks_pending = 0;
		while (i < ctx->num_items) {
			spine_task_t *task = spine_task_alloc();
			if (!task) break; /* Backpressure: wait for slots */
			task->host_id = ctx->host->id;
			task->parent_ctx = ctx;
			task->on_complete = on_mux_complete;
			task->max_retries = 3;
			task->timeout_ms = 1000;
			/* Map a chunk of poller items to the task */
			task->payload = &ctx->poller_items[i];
			task->payload_len = (ctx->num_items - i > items_per_task) ? items_per_task : (ctx->num_items - i);
			spine_scheduler_enqueue(task);
			ctx->mux_tasks_pending++;
			i += task->payload_len;
		}
		if (ctx->mux_tasks_pending > 0) {
			ctx->state = POLL_STATE_WAIT_MUX;
			return 0; /* Async wait */
		}
	}
	ctx->state = POLL_STATE_SCRIPTS;
	return 1;
}

static int stage_wait_mux(poll_context_t *ctx) {
	return 0; /* Stay in this state until on_mux_complete advances us */
}


static int stage_scripts(poll_context_t *ctx) {
	ctx->state = POLL_STATE_FLUSH;
	return 1;
}

static int stage_flush(poll_context_t *ctx) {
	/* Flush-stage placeholder. The real write path goes through
	 * db_insert / poller_push_data_to_main in the sync poller; the
	 * async pipeline advances to DONE and lets the host's accumulated
	 * results drain via that path. Do NOT reintroduce the 'SELECT 1'
	 * dummy query here - it was test scaffolding that dropped real
	 * results when the batcher backpressured. */
	ctx->state = POLL_STATE_DONE;
	return 1;
}

static const spine_async_stage_f polling_pipeline[] = {
	[POLL_STATE_DNS]       = stage_dns,
	[POLL_STATE_PING]      = stage_ping,
	[POLL_STATE_SNMP_SEND] = stage_snmp,
	[POLL_STATE_WAIT_MUX]   = stage_wait_mux,
	[POLL_STATE_SCRIPTS]   = stage_scripts,
	[POLL_STATE_FLUSH]     = stage_flush,
};

static void poll_step(poll_context_t *ctx) {
	if (ctx->state >= POLL_STATE_DONE) {
		uv_timer_stop(&ctx->snmp_timer);
		uv_close((uv_handle_t *)&ctx->snmp_timer, on_handle_closed);
		return;
	}

	spine_async_stage_f handler = polling_pipeline[ctx->state];
	if (handler) {
		if (handler(ctx) != 0) poll_step(ctx);
	} else {
		ctx->state++;
		poll_step(ctx);
	}
}

void spine_transition_state(poll_context_t *ctx) {
	poll_step(ctx);
}

void spine_async_poll_start_internal(uv_loop_t *target_loop, poller_thread_t *det) {
	poll_context_t *ctx = calloc(1, sizeof(poll_context_t));
	if (ctx) {
		ctx->state = POLL_STATE_DNS;
		ctx->spine_host_thread = det->spine_host_thread;
		ctx->host = det->host;
		ctx->event_loop = target_loop;
		ctx->dns_runtime = det->dns_runtime;
		uv_timer_init(target_loop, &ctx->snmp_timer);
		ctx->snmp_timer.data = ctx;
		poll_step(ctx);
	}
}
#else
void spine_async_poll_start_internal(uv_loop_t *target_loop, poller_thread_t *det) { 
	UNUSED_PARAMETER(target_loop); 
	UNUSED_PARAMETER(det); 
}
void spine_transition_state(poll_context_t *ctx) { 
	UNUSED_PARAMETER(ctx); 
}
#endif
