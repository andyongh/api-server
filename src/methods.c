#include "methods.h"
#include "task_manager.h"
#include "jsonrpc.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>
#include <time.h>

worker_pool_t *g_worker_pool = NULL;

/* ══════════════════════════════════════════════════════════════════════
   SYNC METHODS
   ══════════════════════════════════════════════════════════════════════ */

/* ping → {"pong": true, "ts": <epoch>} */
static char *method_ping(const user_info_t *user, yyjson_val *params,
                          int *err_code, char *err_msg, size_t esz) {
    (void)user; (void)params; (void)err_code; (void)err_msg; (void)esz;
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"pong\":true,\"ts\":%ld}", (long)time(NULL));
    return strdup(buf);
}

/* echo → {"echo": <params>, "user": "<username>"} */
static char *method_echo(const user_info_t *user, yyjson_val *params,
                          int *err_code, char *err_msg, size_t esz) {
    (void)err_code; (void)err_msg; (void)esz;
    size_t plen;
    char  *pjson = yyjson_val_write(params, 0, &plen);
    if (!pjson) return strdup("{\"echo\":null}");

    int n = snprintf(NULL, 0,
        "{\"echo\":%s,\"user\":\"%s\"}", pjson, user->username);
    char *buf = malloc(n + 1);
    snprintf(buf, n + 1, "{\"echo\":%s,\"user\":\"%s\"}", pjson, user->username);
    free(pjson);
    return buf;
}

/* add params:{a, b} → {"sum": a+b} */
static char *method_add(const user_info_t *user, yyjson_val *params,
                         int *err_code, char *err_msg, size_t esz) {
    (void)user;
    if (!params || !yyjson_is_obj(params)) {
        *err_code = JRPC_ERR_INVALID_PARAMS;
        snprintf(err_msg, esz, "params must be an object with 'a' and 'b'");
        return NULL;
    }
    yyjson_val *va = yyjson_obj_get(params, "a");
    yyjson_val *vb = yyjson_obj_get(params, "b");
    if (!va || !vb || !yyjson_is_num(va) || !yyjson_is_num(vb)) {
        *err_code = JRPC_ERR_INVALID_PARAMS;
        snprintf(err_msg, esz, "'a' and 'b' must be numbers");
        return NULL;
    }
    double a_val = yyjson_is_int(va) ? (double)yyjson_get_sint(va) : yyjson_get_real(va);
    double b_val = yyjson_is_int(vb) ? (double)yyjson_get_sint(vb) : yyjson_get_real(vb);
    double sum = a_val + b_val;
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"sum\":%g}", sum);
    return strdup(buf);
}

/* ── task.status ───────────────────────────────────────────────────── */
static char *method_task_status(const user_info_t *user, yyjson_val *params,
                                 int *err_code, char *err_msg, size_t esz) {
    if (!params) { *err_code = JRPC_ERR_INVALID_PARAMS; snprintf(err_msg, esz, "params required"); return NULL; }
    yyjson_val *vtid = yyjson_obj_get(params, "task_id");
    if (!vtid || !yyjson_is_str(vtid)) {
        *err_code = JRPC_ERR_INVALID_PARAMS;
        snprintf(err_msg, esz, "'task_id' string required");
        return NULL;
    }
    const char *task_id = yyjson_get_str(vtid);

    task_snapshot_t snap;
    if (!task_snapshot(task_id, &snap)) {
        *err_code = JRPC_ERR_TASK_NOTFOUND;
        snprintf(err_msg, esz, "Task not found: %s", task_id);
        return NULL;
    }
    /* Only owner may inspect (admins skip check) */
    if (strcmp(snap.username, user->username) != 0 &&
        strcmp(user->role, "admin") != 0) {
        task_snapshot_free(&snap);
        *err_code = JRPC_ERR_FORBIDDEN;
        snprintf(err_msg, esz, "Access denied");
        return NULL;
    }

    static const char *states[] = {"pending","running","completed","failed","cancelled","timed_out"};
    const char *state_s = (snap.state >= 0 && snap.state <= 5) ? states[snap.state] : "unknown";

    time_t now     = time(NULL);
    long elapsed   = (snap.started_at > 0) ? (long)(now - snap.started_at) : 0;
    long ttl       = (snap.deadline > 0 && now < snap.deadline)
                     ? (long)(snap.deadline - now) : 0;

    int n;
    char *buf;
    if (snap.state == TASK_COMPLETED && snap.result_json) {
        n = snprintf(NULL, 0,
            "{\"task_id\":\"%s\",\"state\":\"%s\",\"method\":\"%s\","
            "\"elapsed\":%ld,\"result\":%s}",
            snap.task_id, state_s, snap.method, elapsed, snap.result_json);
        buf = malloc(n + 1);
        snprintf(buf, n + 1,
            "{\"task_id\":\"%s\",\"state\":\"%s\",\"method\":\"%s\","
            "\"elapsed\":%ld,\"result\":%s}",
            snap.task_id, state_s, snap.method, elapsed, snap.result_json);
    } else if (snap.state == TASK_FAILED || snap.state == TASK_TIMED_OUT ||
               snap.state == TASK_CANCELLED) {
        /* Escape message */
        char safe[512]; size_t j = 0;
        for (size_t i = 0; snap.error_message[i] && j < sizeof(safe)-2; i++) {
            if (snap.error_message[i]=='"') safe[j++]='\\';
            safe[j++] = snap.error_message[i];
        }
        safe[j] = '\0';
        n = snprintf(NULL, 0,
            "{\"task_id\":\"%s\",\"state\":\"%s\",\"method\":\"%s\","
            "\"error_code\":%d,\"error_message\":\"%s\"}",
            snap.task_id, state_s, snap.method, snap.error_code, safe);
        buf = malloc(n + 1);
        snprintf(buf, n + 1,
            "{\"task_id\":\"%s\",\"state\":\"%s\",\"method\":\"%s\","
            "\"error_code\":%d,\"error_message\":\"%s\"}",
            snap.task_id, state_s, snap.method, snap.error_code, safe);
    } else {
        n = snprintf(NULL, 0,
            "{\"task_id\":\"%s\",\"state\":\"%s\",\"method\":\"%s\","
            "\"elapsed\":%ld,\"ttl\":%ld}",
            snap.task_id, state_s, snap.method, elapsed, ttl);
        buf = malloc(n + 1);
        snprintf(buf, n + 1,
            "{\"task_id\":\"%s\",\"state\":\"%s\",\"method\":\"%s\","
            "\"elapsed\":%ld,\"ttl\":%ld}",
            snap.task_id, state_s, snap.method, elapsed, ttl);
    }
    task_snapshot_free(&snap);
    return buf;
}

/* ── task.cancel ───────────────────────────────────────────────────── */
static char *method_task_cancel(const user_info_t *user, yyjson_val *params,
                                 int *err_code, char *err_msg, size_t esz) {
    if (!params) { *err_code = JRPC_ERR_INVALID_PARAMS; snprintf(err_msg, esz, "params required"); return NULL; }
    yyjson_val *vtid = yyjson_obj_get(params, "task_id");
    if (!vtid || !yyjson_is_str(vtid)) {
        *err_code = JRPC_ERR_INVALID_PARAMS; snprintf(err_msg, esz, "'task_id' required"); return NULL;
    }
    if (!task_cancel(yyjson_get_str(vtid), user->username)) {
        *err_code = JRPC_ERR_TASK_NOTFOUND;
        snprintf(err_msg, esz, "Task not found or cannot be cancelled");
        return NULL;
    }
    return strdup("{\"cancelled\":true}");
}

/* ── task.refresh ──────────────────────────────────────────────────── */
static char *method_task_refresh(const user_info_t *user, yyjson_val *params,
                                  int *err_code, char *err_msg, size_t esz) {
    if (!params) { *err_code = JRPC_ERR_INVALID_PARAMS; snprintf(err_msg, esz, "params required"); return NULL; }
    yyjson_val *vtid = yyjson_obj_get(params, "task_id");
    if (!vtid || !yyjson_is_str(vtid)) {
        *err_code = JRPC_ERR_INVALID_PARAMS; snprintf(err_msg, esz, "'task_id' required"); return NULL;
    }
    if (!task_refresh(yyjson_get_str(vtid), user->username)) {
        *err_code = JRPC_ERR_TASK_NOTFOUND;
        snprintf(err_msg, esz, "Task not found, already terminal, or access denied");
        return NULL;
    }
    return strdup("{\"refreshed\":true}");
}

/* ── task.list ─────────────────────────────────────────────────────── */
static char *method_task_list(const user_info_t *user, yyjson_val *params,
                               int *err_code, char *err_msg, size_t esz) {
    (void)params; (void)err_code; (void)err_msg; (void)esz;
    char *arr = task_list_json(user->username);
    if (!arr) return strdup("[]");
    int n = snprintf(NULL, 0, "{\"tasks\":%s}", arr);
    char *buf = malloc(n + 1);
    snprintf(buf, n + 1, "{\"tasks\":%s}", arr);
    free(arr);
    return buf;
}

/* ══════════════════════════════════════════════════════════════════════
   ASYNC METHODS
   ══════════════════════════════════════════════════════════════════════ */

/*
 * slow_compute: params {n: int}
 * Computes sum of Fibonacci numbers up to n (deliberately slow for demo).
 */
typedef struct {
    char       task_id[37];
    user_info_t user;
    char       *params_json;
} async_ctx_t;

static void slow_compute_work(void *arg) {
    async_ctx_t *ctx = arg;
    LOGI("slow_compute task=%s starting", ctx->task_id);
    task_set_running(ctx->task_id);

    /* Parse params_json to extract n */
    int n = 10;
    yyjson_doc *pdoc = yyjson_read(ctx->params_json,
                                    strlen(ctx->params_json), 0);
    if (pdoc) {
        yyjson_val *proot = yyjson_doc_get_root(pdoc);
        yyjson_val *vn    = yyjson_obj_get(proot, "n");
        if (vn && yyjson_is_int(vn))
            n = (int)yyjson_get_sint(vn);
        if (n < 1) n = 1;
        if (n > 40) n = 40;  /* safety cap */
        yyjson_doc_free(pdoc);
    }

    /* Simulate slow work: compute fib(n) */
    long long a = 0, b = 1;
    for (int i = 0; i < n; i++) {
        long long c = a + b; a = b; b = c;
        sleep(1);  /* 1 s per step */

        /* Check if task was cancelled / timed out during work */
        task_snapshot_t snap;
        if (task_snapshot(ctx->task_id, &snap)) {
            bool cancelled = (snap.state == TASK_CANCELLED ||
                              snap.state == TASK_TIMED_OUT);
            task_snapshot_free(&snap);
            if (cancelled) {
                LOGI("slow_compute task=%s aborted (cancelled/timed-out)",
                     ctx->task_id);
                free(ctx->params_json);
                free(ctx);
                return;
            }
        }
    }

    char result[128];
    snprintf(result, sizeof(result),
             "{\"n\":%d,\"fib_n\":%lld}", n, a);
    task_complete(ctx->task_id, strdup(result));
    LOGI("slow_compute task=%s done fib(%d)=%lld", ctx->task_id, n, a);

    free(ctx->params_json);
    free(ctx);
}

static void method_slow_compute_async(const char *task_id,
                                       const user_info_t *user,
                                       const char *params_json) {
    async_ctx_t *ctx = malloc(sizeof(*ctx));
    memcpy(ctx->task_id, task_id, 37);
    ctx->user        = *user;
    ctx->params_json = strdup(params_json ? params_json : "{}");

    /* Dispatch into the global worker pool (re-uses same pool) */
    if (!worker_pool_submit(g_worker_pool, slow_compute_work, ctx)) {
        task_fail(task_id, JRPC_ERR_QUEUE_FULL, "Worker queue full");
        free(ctx->params_json);
        free(ctx);
    }
}

/* ══════════════════════════════════════════════════════════════════════
   REGISTRY
   ══════════════════════════════════════════════════════════════════════ */

static method_entry_t g_methods[] = {
    { "ping",          false, 0,   method_ping,          NULL                      },
    { "echo",          false, 0,   method_echo,          NULL                      },
    { "add",           false, 0,   method_add,           NULL                      },
    { "slow_compute",  true,  60,  NULL,                 method_slow_compute_async },
    { "task.status",   false, 0,   method_task_status,   NULL                      },
    { "task.cancel",   false, 0,   method_task_cancel,   NULL                      },
    { "task.refresh",  false, 0,   method_task_refresh,  NULL                      },
    { "task.list",     false, 0,   method_task_list,     NULL                      },
    { NULL,            false, 0,   NULL,                 NULL                      },
};

void methods_init(void) {
    LOGI("method registry: %zu methods registered",
         sizeof(g_methods)/sizeof(g_methods[0]) - 1);
}

const method_entry_t *methods_lookup(const char *name) {
    if (!name) return NULL;
    for (int i = 0; g_methods[i].name; i++)
        if (strcmp(g_methods[i].name, name) == 0)
            return &g_methods[i];
    return NULL;
}
