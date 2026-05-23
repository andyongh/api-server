/*
 * rpc_debug — JSONRPC 2.0 debug client
 *
 * Usage:
 *   rpc_debug [options] <method> [params_json]
 *
 * Options:
 *   -h <host>       Server host     (default: 127.0.0.1)
 *   -p <port>       Server port     (default: 8080)
 *   -s <token>      Session token   (default: valid_testuser)
 *   -i <id>         Request ID      (default: 1)
 *   -P              Poll until async task completes
 *   -t <secs>       Poll timeout    (default: 120)
 *   -v              Verbose (show raw HTTP)
 *   -r              Pretty-print JSON response
 *
 * Examples:
 *   rpc_debug ping
 *   rpc_debug echo '{"msg":"hello"}'
 *   rpc_debug add '{"a":3,"b":4}'
 *   rpc_debug -P slow_compute '{"n":5}'
 *   rpc_debug task.status '{"task_id":"<uuid>"}'
 *   rpc_debug task.cancel '{"task_id":"<uuid>"}'
 *   rpc_debug task.list
 *   rpc_debug task.refresh '{"task_id":"<uuid>"}'
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── Config ─────────────────────────────────────────────────────────── */
static const char *g_host    = "127.0.0.1";
static int         g_port    = 8080;
static const char *g_session = "valid_testuser";
static int         g_req_id  = 1;
static bool        g_poll    = false;
static int         g_timeout = 120;
static bool        g_verbose = false;
static bool        g_pretty  = false;

/* ── Minimal dynamic buffer ─────────────────────────────────────────── */
typedef struct { char *data; size_t len; size_t cap; } dbuf_t;

static void dbuf_append(dbuf_t *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2 + 256;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void dbuf_free(dbuf_t *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

/* ── JSON pretty-printer (minimal) ─────────────────────────────────── */
static void pretty_print(const char *json) {
    int depth = 0;
    bool in_string = false;
    for (const char *p = json; *p; p++) {
        if (in_string) {
            putchar(*p);
            if (*p == '\\') { putchar(*++p); continue; }
            if (*p == '"')  in_string = false;
            continue;
        }
        switch (*p) {
        case '"':
            in_string = true; putchar('"'); break;
        case '{': case '[':
            putchar(*p); putchar('\n');
            depth++;
            for (int i = 0; i < depth*2; i++) putchar(' ');
            break;
        case '}': case ']':
            putchar('\n');
            depth--;
            for (int i = 0; i < depth*2; i++) putchar(' ');
            putchar(*p); break;
        case ',':
            putchar(','); putchar('\n');
            for (int i = 0; i < depth*2; i++) putchar(' ');
            break;
        case ':':
            putchar(':'); putchar(' '); break;
        default:
            putchar(*p);
        }
    }
    putchar('\n');
}

/* ── TCP connect ────────────────────────────────────────────────────── */
static int tcp_connect(const char *host, int port) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_s[16];
    snprintf(port_s, sizeof(port_s), "%d", port);

    if (getaddrinfo(host, port_s, &hints, &res) != 0) {
        fprintf(stderr, "ERROR: cannot resolve %s\n", host);
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        fd = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, r->ai_addr, r->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        fprintf(stderr, "ERROR: cannot connect to %s:%d\n", host, port);
    return fd;
}

/* ── HTTP POST and read response ────────────────────────────────────── */
static int http_post(const char *host, int port,
                     const char *body, size_t body_len,
                     dbuf_t *resp_body_out, int *http_status_out) {
    int fd = tcp_connect(host, port);
    if (fd < 0) return -1;

    /* Build request */
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "POST / HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        host, port, body_len);

    if (g_verbose) {
        fprintf(stderr, "\n── REQUEST ──────────────────────────────────\n");
        fprintf(stderr, "%s%s\n", header, body);
    }

    /* Send */
    if (write(fd, header, hlen) < 0 || write(fd, body, body_len) < 0) {
        fprintf(stderr, "ERROR: write failed: %s\n", strerror(errno));
        close(fd); return -1;
    }

    /* Read response */
    dbuf_t raw = {0};
    char chunk[4096];
    ssize_t n;
    while ((n = read(fd, chunk, sizeof(chunk))) > 0)
        dbuf_append(&raw, chunk, (size_t)n);
    close(fd);

    if (g_verbose) {
        fprintf(stderr, "── RESPONSE ─────────────────────────────────\n");
        fprintf(stderr, "%.*s\n", (int)raw.len, raw.data);
        fprintf(stderr, "─────────────────────────────────────────────\n\n");
    }

    /* Parse HTTP status line */
    *http_status_out = 0;
    if (raw.len > 12) {
        const char *sp = strchr(raw.data, ' ');
        if (sp) *http_status_out = atoi(sp + 1);
    }

    /* Find body after \r\n\r\n */
    char *body_start = strstr(raw.data, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        /* Handle chunked transfer (basic: strip chunk sizes) */
        /* For simplicity, we just skip if Content-Length is present */
        size_t blen = raw.len - (size_t)(body_start - raw.data);
        dbuf_append(resp_body_out, body_start, blen);
    }
    dbuf_free(&raw);
    return 0;
}

/* ── Build JSONRPC request ──────────────────────────────────────────── */
static char *build_request(const char *method, const char *params_json,
                            const char *session, int req_id) {
    /* Merge session into params object */
    char merged[4096];
    if (!params_json || strlen(params_json) == 0 ||
        strcmp(params_json, "{}") == 0) {
        snprintf(merged, sizeof(merged),
            "{\"session\":\"%s\"}", session);
    } else {
        /* Insert session before the closing brace */
        size_t plen = strlen(params_json);
        if (params_json[0] == '{' && params_json[plen-1] == '}') {
            /* Check if params is just "{}" or has existing keys */
            char inner[4096 - 64];
            snprintf(inner, sizeof(inner), "%.*s", (int)(plen - 2), params_json + 1);
            /* Remove trailing whitespace */
            size_t ilen = strlen(inner);
            while (ilen > 0 && (inner[ilen-1]==' '||inner[ilen-1]=='\t'||
                                  inner[ilen-1]=='\n'||inner[ilen-1]=='\r'))
                inner[--ilen] = '\0';
            if (ilen > 0)
                snprintf(merged, sizeof(merged),
                    "{\"session\":\"%s\",%s}", session, inner);
            else
                snprintf(merged, sizeof(merged),
                    "{\"session\":\"%s\"}", session);
        } else {
            snprintf(merged, sizeof(merged),
                "{\"session\":\"%s\"}", session);
        }
    }

    int n = snprintf(NULL, 0,
        "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s,\"id\":%d}",
        method, merged, req_id);
    char *buf = malloc(n + 1);
    snprintf(buf, n + 1,
        "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s,\"id\":%d}",
        method, merged, req_id);
    return buf;
}

/* ── Extract JSON string field (minimal parser) ─────────────────────── */
static bool json_get_str(const char *json, const char *key, char *out, size_t outsz) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == ':' || *p == ' ') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < outsz - 1) {
        if (*p == '\\') p++;
        if (*p) out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

/* ── Send one JSONRPC call and print result ─────────────────────────── */
static int __attribute__((unused)) send_call(const char *method, const char *params_json) {
    char *req_body = build_request(method, params_json, g_session, g_req_id);
    size_t req_len = strlen(req_body);

    dbuf_t resp = {0};
    int http_status = 0;
    int rc = http_post(g_host, g_port, req_body, req_len, &resp, &http_status);
    free(req_body);

    if (rc != 0) { dbuf_free(&resp); return 1; }

    printf("HTTP %d\n", http_status);
    if (resp.data && resp.len > 0) {
        if (g_pretty)
            pretty_print(resp.data);
        else
            printf("%s\n", resp.data);
    }

    dbuf_free(&resp);
    return (http_status >= 200 && http_status < 300) ? 0 : 1;
}

/* ── Poll async task until terminal ─────────────────────────────────── */
static int poll_task(const char *task_id) {
    time_t deadline = time(NULL) + g_timeout;
    int interval = 1;

    printf("Polling task %s (timeout=%ds)\n", task_id, g_timeout);

    while (time(NULL) < deadline) {
        sleep(interval);
        if (interval < 5) interval++;

        char params[128];
        snprintf(params, sizeof(params), "{\"task_id\":\"%s\"}", task_id);
        char *req_body = build_request("task.status", params, g_session, ++g_req_id);
        size_t req_len = strlen(req_body);

        dbuf_t resp = {0};
        int http_status = 0;
        int rc = http_post(g_host, g_port, req_body, req_len, &resp, &http_status);
        free(req_body);

        if (rc != 0 || !resp.data) { dbuf_free(&resp); continue; }

        /* Check state in response */
        char state[32] = {0};
        json_get_str(resp.data, "state", state, sizeof(state));

        printf("[%lds] state=%s\n", (long)(time(NULL) - (deadline - g_timeout)),
               *state ? state : "?");

        bool terminal = (strcmp(state, "completed") == 0  ||
                         strcmp(state, "failed")    == 0  ||
                         strcmp(state, "cancelled") == 0  ||
                         strcmp(state, "timed_out") == 0);

        if (terminal) {
            printf("\n── Final result ─────────────────────────────\n");
            if (g_pretty)
                pretty_print(resp.data);
            else
                printf("%s\n", resp.data);
            printf("─────────────────────────────────────────────\n");
            dbuf_free(&resp);
            return 0;
        }
        dbuf_free(&resp);
    }
    fprintf(stderr, "ERROR: poll timeout after %ds\n", g_timeout);
    return 1;
}

/* ── Usage ──────────────────────────────────────────────────────────── */
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] <method> [params_json]\n\n"
        "Options:\n"
        "  -h <host>    Server host    (default: 127.0.0.1)\n"
        "  -p <port>    Server port    (default: 8080)\n"
        "  -s <token>   Session token  (default: valid_testuser)\n"
        "  -i <id>      Request ID     (default: 1)\n"
        "  -P           Poll until async task completes\n"
        "  -t <secs>    Poll timeout   (default: 120)\n"
        "  -v           Verbose (show raw HTTP)\n"
        "  -r           Pretty-print JSON response\n\n"
        "Built-in methods:\n"
        "  ping\n"
        "  echo         '{\"msg\":\"hello\"}'\n"
        "  add          '{\"a\":3,\"b\":4}'\n"
        "  slow_compute '{\"n\":5}'           (async, use -P to poll)\n"
        "  task.status  '{\"task_id\":\"<id>\"}'\n"
        "  task.cancel  '{\"task_id\":\"<id>\"}'\n"
        "  task.refresh '{\"task_id\":\"<id>\"}'\n"
        "  task.list\n\n"
        "Examples:\n"
        "  %s ping\n"
        "  %s -r add '{\"a\":3,\"b\":4}'\n"
        "  %s -P -r slow_compute '{\"n\":5}'\n"
        "  %s -s admin_root task.list\n",
        prog, prog, prog, prog, prog);
}

/* ── Main ───────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "h:p:s:i:Pt:vrH")) != -1) {
        switch (opt) {
        case 'h': g_host    = optarg;        break;
        case 'p': g_port    = atoi(optarg);  break;
        case 's': g_session = optarg;        break;
        case 'i': g_req_id  = atoi(optarg);  break;
        case 'P': g_poll    = true;          break;
        case 't': g_timeout = atoi(optarg);  break;
        case 'v': g_verbose = true;          break;
        case 'r': g_pretty  = true;          break;
        case 'H':
        default:
            usage(argv[0]); return 0;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "ERROR: method required\n\n");
        usage(argv[0]);
        return 1;
    }

    const char *method     = argv[optind];
    const char *params_raw = (optind + 1 < argc) ? argv[optind + 1] : "{}";

    /* Send the call */
    char *req_body = build_request(method, params_raw, g_session, g_req_id);
    size_t req_len = strlen(req_body);

    dbuf_t resp = {0};
    int http_status = 0;
    int rc = http_post(g_host, g_port, req_body, req_len, &resp, &http_status);
    free(req_body);

    if (rc != 0) return 1;

    printf("HTTP %d\n", http_status);

    /* Auto-detect async accepted + polling */
    char status_field[32] = {0};
    char task_id[64]      = {0};
    json_get_str(resp.data ? resp.data : "", "status",  status_field, sizeof(status_field));
    json_get_str(resp.data ? resp.data : "", "task_id", task_id,      sizeof(task_id));

    if (g_pretty && resp.data)
        pretty_print(resp.data);
    else if (resp.data)
        printf("%s\n", resp.data);

    dbuf_free(&resp);

    /* If response says "accepted" and -P flag is set, start polling */
    if (g_poll && strcmp(status_field, "accepted") == 0 && task_id[0]) {
        return poll_task(task_id);
    }

    return (http_status >= 200 && http_status < 300) ? 0 : 1;
}
