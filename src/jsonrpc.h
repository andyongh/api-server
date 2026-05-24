#pragma once
#include "auth.h"
#include "yyjson.h"
#include <stdbool.h>

/* JSONRPC 2.0 error codes */
#define JRPC_PARSE          -32700
#define JRPC_INVALID_REQ    -32600
#define JRPC_METHOD_NF      -32601
#define JRPC_INVALID_PARAMS -32602
#define JRPC_INTERNAL       -32603
/* Application codes */
#define JRPC_AUTH           -32000
#define JRPC_TASK_NF        -32001
#define JRPC_TASK_TIMEOUT   -32002
#define JRPC_QUEUE_FULL     -32003
#define JRPC_CANCELLED      -32004
#define JRPC_FORBIDDEN      -32005

/* Parsed request */
typedef struct {
    char         method[128];
    char         session[256];
    char         id_str[64];     /* JSON literal: "1", "\"s\"", "null" */
    bool         is_notification;
    yyjson_doc  *doc;            /* owner — free with jrpc_req_free()  */
    yyjson_val  *params;         /* ref into doc                       */
    char        *params_json;    /* params serialised without "session" — heap */
    user_info_t  user;           /* filled after auth_verify()         */
} jrpc_req_t;

jrpc_req_t *jrpc_parse(const char *data, size_t len, char **err_resp_out);
void        jrpc_req_free(jrpc_req_t *req);

char *jrpc_result  (const char *id, const char *result_json);
char *jrpc_error   (const char *id, int code, const char *msg);
char *jrpc_accepted(const char *id, const char *task_id, int timeout_secs);