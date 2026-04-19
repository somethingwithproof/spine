#include "common.h"
#include "spine.h"
#include "telemetry.h"

typedef struct {
    uint64_t total_polls;
    double avg_latency_ms;
    int queue_depth;
} spine_metrics_t;

static spine_metrics_t g_metrics = {0};
static uv_pipe_t g_server_pipe;

static void on_telemetry_close(uv_handle_t* handle) {
    (void)handle;
}

static void on_write(uv_write_t* req, int status) {
    (void)status;
    free(req->data);
    free(req);
}

static void on_new_connection(uv_stream_t* server, int status) {
    if (status < 0) return;

    uv_pipe_t* client = malloc(sizeof(uv_pipe_t));
    if (!client) return;

    uv_pipe_init(loop, client, 0);

    if (uv_accept(server, (uv_stream_t*)client) == 0) {
        char buffer[512];
        snprintf(buffer, sizeof(buffer), 
            "{\"status\":\"ok\",\"polls\":%llu,\"latency_avg\":%.2f,\"queue\":%d}\n",
            g_metrics.total_polls, g_metrics.avg_latency_ms, g_metrics.queue_depth);
        
        char *payload = strdup(buffer);
        if (!payload) {
            uv_close((uv_handle_t*)client, (uv_close_cb)free);
            return;
        }

        uv_buf_t res = uv_buf_init(payload, strlen(payload));
        uv_write_t* req = malloc(sizeof(uv_write_t));
        if (!req) {
            free(payload);
            uv_close((uv_handle_t*)client, (uv_close_cb)free);
            return;
        }
        req->data = res.base;
        uv_write(req, (uv_stream_t*)client, &res, 1, on_write);
    } else {
        uv_close((uv_handle_t*)client, (uv_close_cb)free);
    }
}

int spine_telemetry_init(const char *path) {
    uv_pipe_init(loop, &g_server_pipe, 0);
    unlink(path);
    int r = uv_pipe_bind(&g_server_pipe, path);
    if (r) return r;

    r = uv_listen((uv_stream_t*)&g_server_pipe, 128, on_new_connection);
    return r;
}

void spine_telemetry_record_latency(int stage, double ms) {
    (void)stage;
    g_metrics.avg_latency_ms = (g_metrics.avg_latency_ms + ms) / 2.0;
}

void spine_telemetry_add_completed(void) {
    g_metrics.total_polls++;
}

void spine_telemetry_cleanup(void) {
    uv_close((uv_handle_t*)&g_server_pipe, on_telemetry_close);
}
