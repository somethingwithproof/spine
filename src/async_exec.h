#ifndef SPINE_ASYNC_EXEC_H
#define SPINE_ASYNC_EXEC_H

#include <uv.h>

typedef void (*async_exec_cb)(const char *result, int exit_status, int term_signal, void *data);

int spine_async_exec(const char *command, uint64_t timeout_ms, async_exec_cb cb, void *data);

#endif
