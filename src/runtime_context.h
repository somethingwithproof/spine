#ifndef SPINE_RUNTIME_CONTEXT_H
#define SPINE_RUNTIME_CONTEXT_H

#include "common.h"
#include "spine.h"

typedef struct RuntimeConfig {
	const config_t *set;
} RuntimeConfig;

typedef struct RuntimeState {
	double start_time;
} RuntimeState;

typedef struct Logger {
	int (*write)(const char *format, ...);
} Logger;

const RuntimeConfig *runtime_config_current(void);
const RuntimeState *runtime_state_current(void);
const Logger *runtime_logger_current(void);

#endif
