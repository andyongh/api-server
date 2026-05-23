#include "jsonrpc.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Internal helpers ─────────────────────────────────────────────────── */

/* Serialise a yyjson_val id into a short string buffer (JSON literal). */
static void serialise_id(yyjson_val *id_val, char *buf, size_t buflen) {
    if (!id_val || yyjson_is_null(id_val)) {
        snprintf(buf, buflen, "null");
    } else if (yyjson_is_int(id_val)) {
        snprintf(buf, buflen, "%lld", (long long)yyjson_get_sint(id_val));
    } else if (yyjson_is_uint(id_val)) {
        snprintf(buf, buflen, "%llu", (unsigned long long)yyjson_get_uint(id_val));
    } else if (yyjson_is_str(id_val)) {
        const char *s = yyjson_get_str(id_val);
        /* Minimal escape: only handle embedded quotes for safety */
        size_t slen = strlen(s);
        if (slen + 4 < buflen) {
            buf[0] = '"';
            memcpy(buf + 1, s, slen);
            buf[1 + slen] = '"';
            buf[2 + slen] = '\0';
        } else {
            snprintf(buf, buflen, "null");
        }
    } else {
        snprintf(buf, buflen, "null");
    }
}

/*
 * Serialise params object WITHOUT the "session" key into a JSON string.
 * Returns heap-allocated string, caller must free.
 */
static char *params_strip_session(yyjson_val *params) {
    if (!params || !yyjson_is_obj(params))
        return strdup("{}");

    /* Use yyjson mutable doc to rebuild without "session" */
    yyjson_mut_doc  *mdoc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val  *obj  = yyjson_mut_obj(mdoc);
    yyjson_mut_doc_set_root(mdoc, obj);

    yyjson_val *key, *val;
    yyjson_obj_iter iter = yyjson_obj_iter_with(params);
    while ((key = yyjson_obj_iter_next(&iter))) {
        val = yyjson_obj_iter_get_val(key);
        const char *k = yyjson_get_str(key);
        if (k && strcmp(k, "session") == 0) continue;

        /* Re-create key/value in mutable document */
        yyjson_mut_val *mk = yyjson_mut_str(mdoc, k);
        /* We copy the value as raw JSON to avoid deep cloning */
        size_t raw_len;
        char  *raw = yyjson_val_write(val, 0, &raw_len);
        yyjson_mut_val *mv = yyjson_mut_rawn(mdoc, raw, raw_len);
        free(raw);
        yyjson_mut_obj_add(obj, mk, mv);
    }

    size_t  out_len;
    char   *out = yyjson_mut_write(mdoc, 0, &out_len);
    yyjson_mut_doc_free(mdoc);
    return out ? out : strdup("{}");
}

/* ── Public API ───────────────────────────────────────────────────────── */

jrpc_req_t *jrpc_parse(const char *data, size_t len, char **err_out) {
#define ERR(code, msg) do { \
    if (err_out) *err_out = jrpc_build_error("null", code, msg); \
    return NULL; \
} while (0)

    if (!data || len == 0)
        ERR(JRPC_ERR_PARSE, "Empty request body");

    yyjson_read_err read_err;
    yyjson_doc *doc = yyjson_read_opts((char *)data, len,
                                        YYJSON_READ_NOFLAG, NULL, &read_err);
    if (!doc) {
        char msg[128];
        snprintf(msg, sizeof(msg), "JSON parse error: %s", read_err.msg);
        ERR(JRPC_ERR_PARSE, msg);
    }

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        ERR(JRPC_ERR_INVALID_REQ, "Request must be a JSON object");
    }

    /* jsonrpc version */
    yyjson_val *ver = yyjson_obj_get(root, "jsonrpc");
    if (!ver || !yyjson_is_str(ver) ||
        strcmp(yyjson_get_str(ver), "2.0") != 0) {
        yyjson_doc_free(doc);
        ERR(JRPC_ERR_INVALID_REQ, "Missing or invalid 'jsonrpc' field (must be \"2.0\")");
    }

    /* method */
    yyjson_val *method_val = yyjson_obj_get(root, "method");
    if (!method_val || !yyjson_is_str(method_val)) {
        yyjson_doc_free(doc);
        ERR(JRPC_ERR_INVALID_REQ, "Missing or non-string 'method'");
    }
    const char *method_str = yyjson_get_str(method_val);

    /* id (optional) */
    yyjson_val *id_val     = yyjson_obj_get(root, "id");
    bool is_notification   = (id_val == NULL);

    /* params (optional) */
    yyjson_val *params_val = yyjson_obj_get(root, "params");

    /* Extract params.session */
    const char *session_str = NULL;
    if (params_val && yyjson_is_obj(params_val)) {
        yyjson_val *sess = yyjson_obj_get(params_val, "session");
        if (sess && yyjson_is_str(sess))
            session_str = yyjson_get_str(sess);
    }

    if (!session_str) {
        yyjson_doc_free(doc);
        ERR(JRPC_ERR_AUTH, "Missing params.session token");
    }

    /* Build jrpc_req_t */
    jrpc_req_t *req = calloc(1, sizeof(*req));
    if (!req) { yyjson_doc_free(doc); ERR(JRPC_ERR_INTERNAL, "OOM"); }

    snprintf(req->method,  sizeof(req->method),  "%s", method_str);
    snprintf(req->session, sizeof(req->session),  "%s", session_str);
    serialise_id(id_val, req->id_str, sizeof(req->id_str));
    req->is_notification = is_notification;
    req->doc    = doc;
    req->params = params_val;
    req->params_json = params_strip_session(params_val);

    LOGD("jrpc_parse: method=%s id=%s", req->method, req->id_str);
    return req;
#undef ERR
}

void jrpc_req_free(jrpc_req_t *req) {
    if (!req) return;
    yyjson_doc_free(req->doc);
    free(req->params_json);
    free(req);
}

/* ── Response builders ────────────────────────────────────────────────── */

char *jrpc_build_result(const char *id_str, const char *result_json) {
    const char *id   = id_str     ? id_str     : "null";
    const char *res  = result_json ? result_json : "null";
    int  n = snprintf(NULL, 0,
        "{\"jsonrpc\":\"2.0\",\"result\":%s,\"id\":%s}", res, id);
    char *buf = malloc(n + 1);
    if (!buf) return NULL;
    snprintf(buf, n + 1,
        "{\"jsonrpc\":\"2.0\",\"result\":%s,\"id\":%s}", res, id);
    return buf;
}

char *jrpc_build_error(const char *id_str, int code, const char *message) {
    const char *id  = id_str  ? id_str  : "null";
    const char *msg = message ? message : "Internal error";

    /* Escape message quotes defensively */
    char safe_msg[512];
    size_t j = 0;
    for (size_t i = 0; msg[i] && j < sizeof(safe_msg) - 2; i++) {
        if (msg[i] == '"') { safe_msg[j++] = '\\'; }
        safe_msg[j++] = msg[i];
    }
    safe_msg[j] = '\0';

    int n = snprintf(NULL, 0,
        "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":%d,\"message\":\"%s\"},\"id\":%s}",
        code, safe_msg, id);
    char *buf = malloc(n + 1);
    if (!buf) return NULL;
    snprintf(buf, n + 1,
        "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":%d,\"message\":\"%s\"},\"id\":%s}",
        code, safe_msg, id);
    return buf;
}

char *jrpc_build_accepted(const char *id_str,
                           const char *task_id,
                           int timeout_secs) {
    char result[256];
    snprintf(result, sizeof(result),
        "{\"status\":\"accepted\",\"task_id\":\"%s\",\"timeout_secs\":%d}",
        task_id, timeout_secs);
    return jrpc_build_result(id_str, result);
}
