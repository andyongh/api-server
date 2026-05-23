#pragma once
#include "jsonrpc.h"
#include "auth.h"
#include "worker_pool.h"
#include <stdbool.h>

/* ── Handler types ───────────────────────────────────────────────────── */

/**
 * Sync handler — called from a worker thread.
 * Must return a heap-allocated JSON string for the "result" value,
 * or NULL on error (set *err_code and fill err_msg).
 */
typedef char *(*sync_handler_fn)(const user_info_t *user,
                                  yyjson_val *params,
                                  int *err_code,
                                  char *err_msg, size_t err_msg_sz);

/**
 * Async worker — called from a worker thread after the HTTP response
 * (ACCEPTED) has already been sent.
 * Must call task_complete() or task_fail() before returning.
 */
typedef void (*async_handler_fn)(const char *task_id,
                                  const user_info_t *user,
                                  const char *params_json);

/* ── Method entry ────────────────────────────────────────────────────── */
typedef struct {
    const char       *name;
    bool              is_async;
    int               timeout_secs;   /* default timeout for async tasks  */
    sync_handler_fn   sync_fn;        /* NULL if is_async                 */
    async_handler_fn  async_fn;       /* NULL if !is_async                */
} method_entry_t;

/* ── Registry ────────────────────────────────────────────────────────── */

/** Initialise the method registry (registers built-in methods). */
void methods_init(void);

/** Look up a method by name.  Returns NULL if not found. */
const method_entry_t *methods_lookup(const char *name);

/* ── Global worker pool (set before starting server) ─────────────────── */
extern worker_pool_t *g_worker_pool;