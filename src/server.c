#include "server.h"
#include "jsonrpc.h"
#include "auth.h"
#include "methods.h"
#include "task_manager.h"
#include "log.h"
#include <microhttpd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Per-connection state ─────────────────────────────────────────────── */
typedef enum {
    CI_READING    = 0,  /* accumulating request body                     */
    CI_PROCESSING = 1,  /* suspended; worker is running                  */
    CI_READY      = 2,  /* worker done; response ready to send           */
} ci_state_t;

typedef struct conn_info_s {
    volatile ci_state_t  state;
    struct MHD_Connection *mhd_conn;

    /* Request accumulation */
    char   *req_body;
    size_t  req_body_len;
    size_t  req_body_cap;
    bool    body_too_large;

    /* Response (set by worker before resume) */
    char   *resp_body;      /* heap — MUST_FREE so MHD frees it         */
    size_t  resp_body_len;
    int     http_status;
} conn_info_t;

/* ── Sync-dispatch work context ───────────────────────────────────────── */
typedef struct {
    conn_info_t   *ci;
    jrpc_req_t    *req;
    const method_entry_t *entry;
} sync_work_ctx_t;

/* ── Helpers ──────────────────────────────────────────────────────────── */
static void add_common_headers(struct MHD_Response *resp) {
    MHD_add_response_header(resp, "Content-Type", "application/json");
    MHD_add_response_header(resp, "Access-Control-Allow-Origin", "*");
    MHD_add_response_header(resp, "Access-Control-Allow-Headers",
                            "Content-Type");
}

static enum MHD_Result send_json(struct MHD_Connection *conn,
                                  int status, char *body_heap) {
    /* body_heap ownership passes to MHD (MUST_FREE) */
    size_t len = strlen(body_heap);
    struct MHD_Response *resp =
        MHD_create_response_from_buffer(len, body_heap, MHD_RESPMEM_MUST_FREE);
    if (!resp) { free(body_heap); return MHD_NO; }
    add_common_headers(resp);
    enum MHD_Result ret = MHD_queue_response(conn, (unsigned int)status, resp);
    MHD_destroy_response(resp);
    return ret;
}

/* ── Sync worker ──────────────────────────────────────────────────────── */
static void do_sync_work(void *arg) {
    sync_work_ctx_t *ctx = arg;
    conn_info_t     *ci  = ctx->ci;
    jrpc_req_t      *req = ctx->req;
    const method_entry_t *entry = ctx->entry;

    int  err_code = 0;
    char err_msg[512] = {0};

    char *result = entry->sync_fn(&req->user, req->params,
                                   &err_code, err_msg, sizeof(err_msg));
    char *resp_body;
    if (result) {
        resp_body = jrpc_build_result(req->id_str, result);
        free(result);
    } else {
        resp_body = jrpc_build_error(req->id_str, err_code, err_msg);
    }

    jrpc_req_free(req);
    free(ctx);

    /* Store response and resume the MHD connection */
    ci->resp_body     = resp_body;
    ci->resp_body_len = resp_body ? strlen(resp_body) : 0;
    ci->http_status   = 200;
    __atomic_store_n((int *)&ci->state, CI_READY, __ATOMIC_SEQ_CST);
    MHD_resume_connection(ci->mhd_conn);
    LOGD("sync work done, connection resumed");
}

/* ── MHD request handler ──────────────────────────────────────────────── */
static enum MHD_Result handle_request(
        void *cls,
        struct MHD_Connection *connection,
        const char *url,
        const char *method,
        const char *version,
        const char *upload_data,
        size_t *upload_data_size,
        void **con_cls)
{
    (void)version; (void)url;
    worker_pool_t *pool = cls;

    /* ── First call: allocate connection state ── */
    if (*con_cls == NULL) {
        /* Handle CORS preflight */
        if (strcmp(method, "OPTIONS") == 0) {
            struct MHD_Response *r =
                MHD_create_response_from_buffer(0, "", MHD_RESPMEM_PERSISTENT);
            add_common_headers(r);
            MHD_add_response_header(r, "Access-Control-Allow-Methods", "POST, OPTIONS");
            MHD_add_response_header(r, "Allow", "POST, OPTIONS");
            enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, r);
            MHD_destroy_response(r);
            return ret;
        }
        /* Only POST is valid for JSONRPC */
        if (strcmp(method, "POST") != 0) {
            return send_json(connection, MHD_HTTP_METHOD_NOT_ALLOWED,
                jrpc_build_error("null", JRPC_ERR_INVALID_REQ,
                                  "Only POST is accepted"));
        }
        conn_info_t *ci = calloc(1, sizeof(*ci));
        if (!ci) return MHD_NO;
        ci->state    = CI_READING;
        ci->mhd_conn = connection;
        ci->http_status = 200;
        *con_cls = ci;
        return MHD_YES;
    }

    conn_info_t *ci = *con_cls;

    /* ── Accumulate request body ── */
    if (*upload_data_size > 0) {
        if (ci->req_body_len + *upload_data_size > MAX_BODY_SIZE) {
            ci->body_too_large = true;
            *upload_data_size = 0;
            return MHD_YES;
        }
        size_t new_len = ci->req_body_len + *upload_data_size;
        if (new_len + 1 > ci->req_body_cap) {
            size_t new_cap = (new_len + 1) * 2;
            char *nb = realloc(ci->req_body, new_cap);
            if (!nb) return MHD_NO;
            ci->req_body     = nb;
            ci->req_body_cap = new_cap;
        }
        memcpy(ci->req_body + ci->req_body_len, upload_data, *upload_data_size);
        ci->req_body_len += *upload_data_size;
        ci->req_body[ci->req_body_len] = '\0';
        *upload_data_size = 0;
        return MHD_YES;
    }

    /* ── Body complete; not yet submitted to worker ── */
    if (ci->state == CI_READING) {

        if (ci->body_too_large) {
            return send_json(connection, MHD_HTTP_CONTENT_TOO_LARGE,
                jrpc_build_error("null", JRPC_ERR_INVALID_REQ, "Request body too large"));
        }
        if (!ci->req_body || ci->req_body_len == 0) {
            return send_json(connection, MHD_HTTP_BAD_REQUEST,
                jrpc_build_error("null", JRPC_ERR_PARSE, "Empty body"));
        }

        /* Parse JSONRPC */
        char *parse_err = NULL;
        jrpc_req_t *req = jrpc_parse(ci->req_body, ci->req_body_len, &parse_err);
        if (!req) {
            LOGW("jrpc_parse failed");
            return send_json(connection, MHD_HTTP_BAD_REQUEST, parse_err);
        }

        /* Authenticate */
        if (!auth_verify(req->session, &req->user)) {
            LOGW("auth failed for session='%.32s'", req->session);
            char *err = jrpc_build_error(req->id_str, JRPC_ERR_AUTH,
                                          "Authentication failed");
            jrpc_req_free(req);
            return send_json(connection, MHD_HTTP_UNAUTHORIZED, err);
        }
        LOGD("auth ok user=%s method=%s", req->user.username, req->method);

        /* Look up method */
        const method_entry_t *entry = methods_lookup(req->method);
        if (!entry) {
            char err_msg[256];
            snprintf(err_msg, sizeof(err_msg), "Method not found: %s", req->method);
            char *err = jrpc_build_error(req->id_str, JRPC_ERR_METHOD_NOTFOUND, err_msg);
            jrpc_req_free(req);
            return send_json(connection, MHD_HTTP_OK, err);
        }

        /* ── Async path: return ACCEPTED immediately ── */
        if (entry->is_async) {
            char task_id[37];
            task_t *task = task_create(req->method, &req->user,
                                        req->params_json,
                                        entry->timeout_secs,
                                        task_id);
            if (!task) {
                char *err = jrpc_build_error(req->id_str, JRPC_ERR_INTERNAL, "OOM");
                jrpc_req_free(req);
                return send_json(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, err);
            }

            /* Submit async work to pool */
            entry->async_fn(task_id, &req->user, req->params_json);
            LOGI("async task created id=%s method=%s user=%s",
                 task_id, req->method, req->user.username);

            char *accepted = jrpc_build_accepted(req->id_str, task_id,
                                                   entry->timeout_secs);
            jrpc_req_free(req);
            return send_json(connection, MHD_HTTP_ACCEPTED, accepted);
        }

        /* ── Sync path: suspend connection, dispatch to worker ── */
        sync_work_ctx_t *ctx = malloc(sizeof(*ctx));
        if (!ctx) {
            char *err = jrpc_build_error(req->id_str, JRPC_ERR_INTERNAL, "OOM");
            jrpc_req_free(req);
            return send_json(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, err);
        }
        ctx->ci    = ci;
        ctx->req   = req;
        ctx->entry = entry;

        ci->state = CI_PROCESSING;
        MHD_suspend_connection(connection);

        if (!worker_pool_submit(pool, do_sync_work, ctx)) {
            /* Queue full — resume immediately with error */
            jrpc_req_free(req);
            free(ctx);
            ci->resp_body = jrpc_build_error(req->id_str,
                                              JRPC_ERR_QUEUE_FULL,
                                              "Server busy, retry later");
            ci->resp_body_len = strlen(ci->resp_body);
            ci->http_status   = MHD_HTTP_SERVICE_UNAVAILABLE;
            __atomic_store_n((int *)&ci->state, CI_READY, __ATOMIC_SEQ_CST);
            MHD_resume_connection(connection);
        }
        return MHD_YES;
    }

    /* ── Worker done; state == CI_READY; send response ── */
    if (ci->state == CI_READY) {
        if (!ci->resp_body) {
            ci->resp_body     = strdup("{\"error\":\"internal\"}");
            ci->resp_body_len = strlen(ci->resp_body);
        }
        return send_json(connection, ci->http_status, ci->resp_body);
        /* NB: ci->resp_body ownership transferred to MHD via MUST_FREE.
         * Do NOT free ci->resp_body here. */
    }

    LOGE("handle_request: unexpected state %d", (int)ci->state);
    return MHD_NO;
}

/* ── Connection cleanup ───────────────────────────────────────────────── */
static void on_request_done(void *cls,
                             struct MHD_Connection *connection,
                             void **con_cls,
                             enum MHD_RequestTerminationCode toe) {
    (void)cls; (void)connection; (void)toe;
    conn_info_t *ci = *con_cls;
    if (!ci) return;
    free(ci->req_body);
    /* resp_body was transferred to MHD (MUST_FREE) — already freed */
    free(ci);
    *con_cls = NULL;
}

/* ── Public API ───────────────────────────────────────────────────────── */
struct MHD_Daemon *server_start(uint16_t port,
                                 unsigned int thread_count,
                                 worker_pool_t *pool) {
    unsigned int flags =
        MHD_USE_INTERNAL_POLLING_THREAD |
        MHD_ALLOW_SUSPEND_RESUME        |
        MHD_USE_ERROR_LOG;

    /* Use epoll on Linux for better performance */
#ifdef MHD_USE_EPOLL
    flags |= MHD_USE_EPOLL;
#endif

    struct MHD_Daemon *d = MHD_start_daemon(
        flags, port,
        NULL, NULL,                         /* accept policy (all)         */
        handle_request, pool,               /* handler + cls               */
        MHD_OPTION_THREAD_POOL_SIZE,  thread_count,
        MHD_OPTION_CONNECTION_LIMIT,  (unsigned int)MAX_CONCURRENT_CONNS,
        MHD_OPTION_CONNECTION_TIMEOUT,(unsigned int)CONN_TIMEOUT_SECS,
        MHD_OPTION_NOTIFY_COMPLETED,  on_request_done, NULL,
        MHD_OPTION_END);

    if (!d) {
        LOGE("MHD_start_daemon failed on port %d", (int)port);
        return NULL;
    }
    LOGI("server listening on port %d (threads=%u, max_conn=%d)",
         (int)port, thread_count, MAX_CONCURRENT_CONNS);
    return d;
}

void server_stop(struct MHD_Daemon *d) {
    if (d) {
        MHD_stop_daemon(d);
        LOGI("server stopped");
    }
}
