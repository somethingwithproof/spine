#include "common.h"
#include "spine.h"
#include "async_exec.h"

typedef struct {
    uv_process_t process;
    uv_pipe_t stdout_pipe;
    uv_timer_t timer;
    char *result_buffer;
    size_t buffer_pos;
    async_exec_cb callback;
    void *data;
    bool timed_out;
    int closed_handles;
} async_exec_ctx_t;

static void on_close(uv_handle_t *handle) {
    async_exec_ctx_t *ctx = (async_exec_ctx_t *)handle->data;
    
    ctx->closed_handles++;
    if (ctx->closed_handles == 3) {
        free(ctx->result_buffer);
        free(ctx);
    }
}

static void on_stdout_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    async_exec_ctx_t *ctx = (async_exec_ctx_t *)stream->data;

    if (nread > 0) {
        size_t available = RESULTS_BUFFER - ctx->buffer_pos - 1;
        size_t to_copy = (size_t)nread < available ? (size_t)nread : available;
        
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
    buf->len = suggested_size;
}

static void on_process_exit(uv_process_t *process, int64_t exit_status, int term_signal) {
    async_exec_ctx_t *ctx = (async_exec_ctx_t *)process->data;
    
    uv_timer_stop(&ctx->timer);
    uv_read_stop((uv_stream_t *)&ctx->stdout_pipe);

    if (!ctx->timed_out) {
        ctx->callback(ctx->result_buffer, (int)exit_status, term_signal, ctx->data);
    }

    uv_close((uv_handle_t *)process, on_close);
    uv_close((uv_handle_t *)&ctx->stdout_pipe, on_close);
    uv_close((uv_handle_t *)&ctx->timer, on_close);
}

static void on_timeout(uv_timer_t *handle) {
    async_exec_ctx_t *ctx = (async_exec_ctx_t *)handle->data;
    ctx->timed_out = true;
    
    uv_process_kill(&ctx->process, SIGKILL);
    ctx->callback(NULL, -1, SIGKILL, ctx->data);
}

int spine_async_exec(const char *command, uint64_t timeout_ms, async_exec_cb cb, void *data) {
    async_exec_ctx_t *ctx = calloc(1, sizeof(async_exec_ctx_t));
    if (!ctx) return -ENOMEM;

    ctx->result_buffer = malloc(RESULTS_BUFFER);
    if (!ctx->result_buffer) {
        free(ctx);
        return -ENOMEM;
    }
    ctx->result_buffer[0] = '\0';
    ctx->callback = cb;
    ctx->data = data;

    uv_pipe_init(loop, &ctx->stdout_pipe, 0);
    ctx->stdout_pipe.data = ctx;

    uv_timer_init(loop, &ctx->timer);
    ctx->timer.data = ctx;
    uv_timer_start(&ctx->timer, on_timeout, timeout_ms, 0);

    uv_stdio_container_t stdio[3];
    stdio[0].flags = UV_IGNORE;
    stdio[1].flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE;
    stdio[1].data.stream = (uv_stream_t *)&ctx->stdout_pipe;
    stdio[2].flags = UV_IGNORE;

    char *args[4];
    args[0] = "/bin/sh";
    args[1] = "-c";
    args[2] = (char *)command;
    args[3] = NULL;

    uv_process_options_t options = {0};
    options.exit_cb = on_process_exit;
    options.file = args[0];
    options.args = args;
    options.stdio_count = 3;
    options.stdio = stdio;

    int r = uv_spawn(loop, &ctx->process, &options);
    if (r) {
        uv_timer_stop(&ctx->timer);
        ctx->closed_handles = 1; // Mark process as closed since it didn't spawn
        uv_close((uv_handle_t *)&ctx->stdout_pipe, on_close);
        uv_close((uv_handle_t *)&ctx->timer, on_close);
        return r;
    }
    
    ctx->process.data = ctx;
    uv_read_start((uv_stream_t *)&ctx->stdout_pipe, on_alloc, on_stdout_read);

    return 0;
}
