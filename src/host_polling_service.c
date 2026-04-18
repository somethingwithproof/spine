#include "common.h"
#include "spine.h"
#include "host_polling_service.h"

static HostPollingStageFn host_polling_stage_fn(const HostPollingRequest *request, HostPollingStage stage) {
	switch (stage) {
	case HOST_POLL_STAGE_LOAD_WORK_ITEMS:
		return request->on_load_work_items;
	case HOST_POLL_STAGE_CHECK_AVAILABILITY:
		return request->on_check_availability;
	case HOST_POLL_STAGE_POLL_ITEMS:
		return request->on_poll_items;
	case HOST_POLL_STAGE_PERSIST_RESULTS:
		return request->on_persist_results;
	case HOST_POLL_STAGE_UPDATE_HOST_STATE:
		return request->on_update_host_state;
	default:
		return NULL;
	}
}

static ResultCode host_polling_execute_stage(const HostPollingRequest *request, HostPollingStage stage,
	HostPollingResult *result, int retries_allowed) {
	HostPollingStageFn fn;
	HostPollingStageOutput output;
	int attempt = 0;

	fn = host_polling_stage_fn(request, stage);
	if (fn == NULL) {
		return RESULT_CODE_OK;
	}

	memset(&output, 0, sizeof(output));

	do {
		memset(&output, 0, sizeof(output));
		output.code = fn(request, &output);
		if (output.code == RESULT_CODE_OK) {
			if (output.host_errors > 0) {
				result->host_errors = output.host_errors;
			}
			return RESULT_CODE_OK;
		}

		if (!output.retryable || attempt >= retries_allowed) {
			result->failed_stage = stage;
			return RESULT_CODE_ERROR;
		}

		attempt++;
		result->retries_used++;
	} while (1);
}

HostPollingResult host_polling_service_run(const HostPollingRequest *request, const spine_services_t *services) {
	HostPollingResult result = {0};

	UNUSED_PARAMETER(services);

	result.failed_stage = HOST_POLL_STAGE_UPDATE_HOST_STATE;

	if (request == NULL) {
		result.code = RESULT_CODE_ERROR;
		result.host_errors = 1;
		result.failed_stage = HOST_POLL_STAGE_LOAD_WORK_ITEMS;
		return result;
	}

	if (host_polling_execute_stage(request, HOST_POLL_STAGE_LOAD_WORK_ITEMS, &result, 0) != RESULT_CODE_OK) {
		result.code = RESULT_CODE_ERROR;
		return result;
	}
	if (host_polling_execute_stage(request, HOST_POLL_STAGE_CHECK_AVAILABILITY, &result, 0) != RESULT_CODE_OK) {
		result.code = RESULT_CODE_ERROR;
		return result;
	}
	if (host_polling_execute_stage(request, HOST_POLL_STAGE_POLL_ITEMS, &result, request->max_retries) != RESULT_CODE_OK) {
		result.code = RESULT_CODE_ERROR;
		return result;
	}
	if (host_polling_execute_stage(request, HOST_POLL_STAGE_PERSIST_RESULTS, &result, 0) != RESULT_CODE_OK) {
		result.code = RESULT_CODE_ERROR;
		return result;
	}
	if (host_polling_execute_stage(request, HOST_POLL_STAGE_UPDATE_HOST_STATE, &result, 0) != RESULT_CODE_OK) {
		result.code = RESULT_CODE_ERROR;
		return result;
	}

	result.code = RESULT_CODE_OK;
	return result;
}
