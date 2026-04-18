#include "common.h"
#include "spine.h"
#include "async_mysql.h"
#include "async_batch.h"

#define BATCH_BUFFER_SIZE (MAX_MYSQL_BUF_SIZE * 2)

typedef struct {
    MYSQL *mysql;
    uv_timer_t flush_timer;
    char *query_buffer;
    int current_batch_size;
    int max_batch_size;
    int flush_interval_ms;
    int active_queries;
    bool initialized;
} async_batch_ctx_t;

static async_batch_ctx_t g_batch_ctx = {0};

static void batch_query_cb(MYSQL *mysql, int status, void *data) {
    (void)mysql;
    (void)data;
    
    if (status != 0) {
        SPINE_LOG(("ERROR: Async batch flush failed with status %d", status));
    } else {
        SPINE_LOG_DEVDBG(("DEBUG: Async batch flush succeeded"));
    }
    
    g_batch_ctx.active_queries--;
}

static void do_flush(void) {
    if (g_batch_ctx.current_batch_size == 0) return;

    // We need to copy the buffer because spine_async_mysql_query is asynchronous
    // and might outlive the next batch.
    char *query_copy = strdup(g_batch_ctx.query_buffer);
    if (query_copy) {
        g_batch_ctx.active_queries++;
        int r = spine_async_mysql_query(g_batch_ctx.mysql, query_copy, batch_query_cb, NULL);
        if (r != 0) {
            free(query_copy);
            g_batch_ctx.active_queries--;
        }
    }

    // Reset buffer
    g_batch_ctx.query_buffer[0] = '\0';
    g_batch_ctx.current_batch_size = 0;
}

static void on_flush_timer(uv_timer_t *handle) {
    (void)handle;
    do_flush();
}

int spine_async_batch_init(MYSQL *mysql, int max_batch_size, int flush_interval_ms) {
    if (g_batch_ctx.initialized) return 0;

    g_batch_ctx.mysql = mysql;
    g_batch_ctx.max_batch_size = max_batch_size;
    g_batch_ctx.flush_interval_ms = flush_interval_ms;
    g_batch_ctx.query_buffer = malloc(BATCH_BUFFER_SIZE);
    g_batch_ctx.query_buffer[0] = '\0';
    g_batch_ctx.current_batch_size = 0;
    g_batch_ctx.active_queries = 0;

    uv_timer_init(loop, &g_batch_ctx.flush_timer);
    uv_timer_start(&g_batch_ctx.flush_timer, on_flush_timer, flush_interval_ms, flush_interval_ms);

    g_batch_ctx.initialized = true;
    return 0;
}

int spine_async_batch_enqueue(const char *query) {
    if (!g_batch_ctx.initialized) return -1;

    size_t current_len = strlen(g_batch_ctx.query_buffer);
    size_t new_len = strlen(query);

    // Simple concatenation. For real Cacti, this should construct a multi-row INSERT.
    // For this skeleton, we just append with a semicolon.
    if (current_len + new_len + 2 < BATCH_BUFFER_SIZE) {
        if (current_len > 0) {
            strcat(g_batch_ctx.query_buffer, ";");
        }
        strcat(g_batch_ctx.query_buffer, query);
        g_batch_ctx.current_batch_size++;

        if (g_batch_ctx.current_batch_size >= g_batch_ctx.max_batch_size) {
            do_flush();
        }
        return 0;
    }

    return -1; // Buffer overflow
}

void spine_async_batch_flush(void) {
    if (!g_batch_ctx.initialized) return;
    do_flush();
}

static void on_batch_close(uv_handle_t *handle) {
    (void)handle;
    free(g_batch_ctx.query_buffer);
    g_batch_ctx.query_buffer = NULL;
    g_batch_ctx.initialized = false;
}

void spine_async_batch_cleanup(void) {
    if (!g_batch_ctx.initialized) return;
    uv_timer_stop(&g_batch_ctx.flush_timer);
    uv_close((uv_handle_t *)&g_batch_ctx.flush_timer, on_batch_close);
}
