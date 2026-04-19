#include "common.h"
#include "spine.h"
#include "async_exec.h"

/*
 * Async process exec via libuv. Callback fires exactly once; handles
 * are torn down deterministically through a three-handle close count
 * with a terminal flag to prevent the earlier double-callback and
 * double-free race between on_timeout and on_process_exit.
 *
 * Commands still run under /bin/sh -c to match nft_popen.c's existing
 * behaviour (Cacti poller items may contain shell metacharacters by
 * design). The shell boundary is the same as the legacy sync path; no
 * new injection primitive is introduced.
 */

typedef struct {
    uv_process_t process;
    uv_pipe_t    stdout_pipe;
    uv_timer_t   timer;
    char        *result_buffer;
    size_t       buffer_pos;
    async_exec_cb callback;
    void        *data;
    int          exit_status;
    int          term_signal;
    bool         timed_out;
    bool         callback_fired;
    bool         process_started;
    int          pending_closes;
} async_exec_ctx_t;

static void async_exec_fire_callback(async_exec_ctx_t *ctx) {
    if (ctx->callback_fired || !ctx->callback) return;
    ctx->callback_fired = true;

    if (ctx->timed_out) {
        ctx->callback(NULL, -1, SIGKILL, ctx->data);
    } else {
        ctx->callback(ctx->result_buffer, ctx->exit_status, ctx->term_signal, ctx->data);
    }
}

static void on_close(uv_handle_t *handle) {
    async_exec_ctx_t *ctx = (async_exec_ctx_t *)handle->data;
    if (!ctx) return;

    if (--ctx->pending_closes == 0) {
        async_exec_fire_callback(ctx);
        free(ctx->result_buffer);
        free(ctx);
    }
}

static void async_exec_close_all(async_exec_ctx_t *ctx) {
    /* Idempotent teardown: uv_close on an already-closing handle is
     * a libuv abort, so only close handles we know are live. */
    if (ctx->process_started && !uv_is_closing((uv_handle_t *)&ctx->process)) {
        uv_close((uv_handle_t *)&ctx->process, on_close);
    } else {
        ctx->pending_closes--;
    }
    if (!uv_is_closing((uv_handle_t *)&ctx->stdout_pipe)) {
        uv_close((uv_handle_t *)&ctx->stdout_pipe, on_close);
    } else {
        ctx->pending_closes--;
    }
    if (!uv_is_closing((uv_handle_t *)&ctx->timer)) {
        uv_close((uv_handle_t *)&ctx->timer, on_close);
    } else {
        ctx->pending_closes--;
    }

    if (ctx->pending_closes == 0) {
        async_exec_fire_callback(ctx);
        free(ctx->result_buffer);
        free(ctx);
    }
}

static void on_stdout_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    async_exec_ctx_t *ctx = (async_exec_ctx_t *)stream->data;

    if (nread > 0 && ctx) {
        size_t available = RESULTS_BUFFER - ctx->buffer_pos - 1;
        size_t to_copy   = (size_t)nread < available ? (size_t)nread : available;
        if (to_copy > 0) {
            memcpy(ctx->result_buffer + ctx->buffer_pos, buf->base, to_copy);
            ctx->buffer_pos += to_copy;
            ctx->result_buffer[ctx->buffer_pos] = '\0';
        }
    }

    if (buf->base) {
        free(buf->base);
    }
}

static void on_alloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    (void)handle;
    buf->base = malloc(suggested_size);
    buf->len  = buf->base ? suggested_size : 0;
}

static void on_process_exit(uv_process_t *process, int64_t exit_status, int term_signal) {
    async_exec_ctx_t *ctx = (async_exec_ctx_t *)process->data;
    if (!ctx) return;

    /* Record outcome but do NOT fire the callback yet; it fires once
     * from the last on_close so every handle is torn down first. */
    if (!ctx->timed_out) {
        ctx->exit_status = (int)exit_status;
        ctx->term_signal = term_signal;
    }

    uv_timer_stop(&ctx->timer);
    uv_read_stop((uv_stream_t *)&ctx->stdout_pipe);

    async_exec_close_all(ctx);
}

static void on_timeout(uv_timer_t *handle) {
    async_exec_ctx_t *ctx = (async_exec_ctx_t *)handle->data;
    if (!ctx || ctx->timed_out) return;

    ctx->timed_out = true;
    /* Ask the process to exit. on_process_exit will then tear handles
     * down via async_exec_close_all; the callback fires once from the
     * last on_close with the timed_out flag producing the -1 result. */
    if (ctx->process_started) {
        uv_process_kill(&ctx->process, SIGKILL);
    } else {
        async_exec_close_all(ctx);
    }
}

int spine_async_exec(const char *command, uint64_t timeout_ms, async_exec_cb cb, void *data) {
    if (!command || !cb) return -EINVAL;

    async_exec_ctx_t *ctx = calloc(1, sizeof(async_exec_ctx_t));
    if (!ctx) return -ENOMEM;

    ctx->result_buffer = malloc(RESULTS_BUFFER);
    if (!ctx->result_buffer) {
        free(ctx);
        return -ENOMEM;
    }
    ctx->result_buffer[0] = '\0';
    ctx->callback        = cb;
    ctx->data            = data;
    ctx->pending_closes  = 3;

    uv_pipe_init(loop, &ctx->stdout_pipe, 0);
    ctx->stdout_pipe.data = ctx;

    uv_timer_init(loop, &ctx->timer);
    ctx->timer.data = ctx;
    uv_timer_start(&ctx->timer, on_timeout, timeout_ms, 0);

    uv_stdio_container_t stdio[3];
    stdio[0].flags       = UV_IGNORE;
    stdio[1].flags       = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
    stdio[1].data.stream = (uv_stream_t *)&ctx->stdout_pipe;
    stdio[2].flags       = UV_IGNORE;

    char *args[4];
    args[0] = "/bin/sh";
    args[1] = "-c";
    args[2] = (char *)command;
    args[3] = NULL;

    uv_process_options_t options = {0};
    options.exit_cb     = on_process_exit;
    options.file        = args[0];
    options.args        = args;
    options.stdio_count = 3;
    options.stdio       = stdio;

    ctx->process.data = ctx;
    int r = uv_spawn(loop, &ctx->process, &options);
    if (r) {
        /* Spawn failed: process handle was not installed. Release pipe
         * and timer, then the ctx. No callback — caller sees r. */
        ctx->process_started = false;
        uv_timer_stop(&ctx->timer);
        ctx->pending_closes = 2;
        uv_close((uv_handle_t *)&ctx->stdout_pipe, on_close);
        uv_close((uv_handle_t *)&ctx->timer, on_close);
        /* Suppress callback since the caller already gets r. */
        ctx->callback_fired = true;
        return r;
    }
    ctx->process_started = true;

    uv_read_start((uv_stream_t *)&ctx->stdout_pipe, on_alloc, on_stdout_read);
    return 0;
}
