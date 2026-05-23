#pragma once
#include "auth.h"
#include "yyjson.h"
#include <stdbool.h>

/* ── JSONRPC 2.0 error codes ─────────────────────────────────────────── */
#define JRPC_ERR_PARSE          -32700
#define JRPC_ERR_INVALID_REQ    -32600
#define JRPC_ERR_METHOD_NOTFOUND -32601
#define JRPC_ERR_INVALID_PARAMS -32602
#define JRPC_ERR_INTERNAL       -32603
/* Application-defined codes (–32000 to –32099) */
#define JRPC_ERR_AUTH           -32000
#define JRPC_ERR_TASK_NOTFOUND  -32001
#define JRPC_ERR_TASK_TIMEOUT   -32002
#define JRPC_ERR_QUEUE_FULL     -32003
#define JRPC_ERR_CANCELLED      -32004
#define JRPC_ERR_FORBIDDEN      -32005

/* ── Parsed request ───────────────────────────────────────────────────── */
typedef struct {
    char         method[128];
    char         session[256];   /* params.session token                  */
    char         id_str[64];     /* JSON-serialised id: "1","\"s\"","null"*/
    bool         is_notification; /* true if id absent (client ignores reply) */

    yyjson_doc  *doc;            /* owning document — must call jrpc_req_free */
    yyjson_val  *params;         /* reference into doc, may be NULL       */

    char        *params_json;    /* serialised params (without "session") */
    user_info_t  user;           /* filled after auth_verify              */
} jrpc_req_t;

/**
 * Parse a JSONRPC 2.0 request from raw JSON bytes.
 * On success returns an allocated jrpc_req_t; caller must call jrpc_req_free().
 * On failure returns NULL and writes an error response into *err_resp_out
 * (heap-allocated, caller must free).
 */
jrpc_req_t *jrpc_parse(const char *data, size_t len, char **err_resp_out);

/** Free a jrpc_req_t and all owned memory. */
void jrpc_req_free(jrpc_req_t *req);

/* ── Response builders ────────────────────────────────────────────────── */

/**
 * Build: {"jsonrpc":"2.0","result":<result_json>,"id":<id_str>}
 * Returns heap-allocated string.  Caller must free.
 * result_json must be a valid JSON value (object/array/scalar).
 */
char *jrpc_build_result(const char *id_str, const char *result_json);

/**
 * Build: {"jsonrpc":"2.0","error":{"code":<c>,"message":"<m>"},"id":<id_str>}
 * id_str may be "null" for parse errors.
 * Returns heap-allocated string.  Caller must free.
 */
char *jrpc_build_error(const char *id_str, int code, const char *message);

/**
 * Convenience: ACCEPTED async response.
 * Returns {"jsonrpc":"2.0","result":{"status":"accepted",
 *           "task_id":"<tid>","timeout_secs":<t>},"id":<id_str>}
 */
char *jrpc_build_accepted(const char *id_str,
                           const char *task_id,
                           int         timeout_secs);