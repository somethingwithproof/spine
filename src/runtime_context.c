#include "common.h"
#include "spine.h"
#include "runtime_context.h"

extern double start_time;

const RuntimeConfig *runtime_config_current(void) {
	static RuntimeConfig config = {0};
	config.set = &set;
	return &config;
}

const RuntimeState *runtime_state_current(void) {
	static RuntimeState state = {0};
	state.start_time = start_time;
	return &state;
}

const Logger *runtime_logger_current(void) {
	static Logger logger = {0};
	logger.write = spine_log;
	return &logger;
}
