#pragma once
#include "jsonrpc.h"
#include "task_manager.h"
#include "worker_pool.h"
#include <stdbool.h>

/*
 * sync_handler_fn — called from worker thread for synchronous methods.
 * Returns heap-allocated result JSON string, or NULL on error
 * (must set *err_code and write err_msg).
 */
typedef char *(*sync_handler_fn)(const user_info_t *user,
                                  yyjson_val *params,
                                  int *err_code,
                                  char *err_msg, size_t err_msg_sz);

/*
 * Method table entry.
 *
 * For async methods, async_fn has type task_work_fn_t = work_fn_t
 * = void (*)(void *arg).  Convention: arg is task_t *; cast inside.
 *
 * This is the only function-pointer type used for async dispatch.
 * There is no separate async_handler_fn typedef.
 */
typedef struct {
    const char      *name;
    bool             is_async;
    int              timeout_secs;
    sync_handler_fn  sync_fn;    /* NULL when is_async */
    task_work_fn_t   async_fn;   /* void(*)(void*); NULL when !is_async */
} method_entry_t;

void                  methods_init(void);
const method_entry_t *methods_lookup(const char *name);

extern worker_pool_t *g_worker_pool;