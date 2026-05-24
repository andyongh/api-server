#pragma once
#include "auth.h"
#include "worker_pool.h"   /* work_fn_t = void(*)(void*) */
#include <time.h>
#include <stdbool.h>
#include <pthread.h>

/* ── States ──────────────────────────────────────────────────────────── */
typedef enum {
    TASK_PENDING   = 0,
    TASK_RUNNING   = 1,
    TASK_COMPLETED = 2,
    TASK_FAILED    = 3,
    TASK_CANCELLED = 4,
    TASK_TIMED_OUT = 5,
} task_state_t;

/*
 * task_work_fn_t — signature for async business-logic functions.
 *
 * Deliberately identical to work_fn_t (void (*)(void *)) so the function
 * pointer can be stored in task_t.fn and submitted to the worker pool
 * without any cast or wrapper.
 *
 * Convention: arg is always a task_t *; cast it at the top of the function.
 */
typedef work_fn_t task_work_fn_t;   /* void (*)(void *arg) */

/* ── Task record ─────────────────────────────────────────────────────── */
typedef struct task_s {
    /* ── scheduling ─────────────────────────────────────────────────── */
    task_work_fn_t  fn;           /* business logic; set before task_submit() */
    void           *fn_arg;       /* optional pre-parsed data; fn owns/frees  */
    struct task_s  *queue_next;   /* intrusive link used by worker queue       */

    /* ── identity & lifecycle ────────────────────────────────────────── */
    char            task_id[37];
    task_state_t    state;
    user_info_t     owner;
    char            method[128];
    char           *params_json;  /* heap, owned by task                       */
    char           *result_json;  /* heap, set on COMPLETED                    */
    int             error_code;
    char            error_message[512];
    time_t          created_at, started_at, finished_at;
    time_t          deadline;     /* 0 = no timeout                            */
    int             timeout_secs;

    pthread_mutex_t mu;           /* protects mutable fields                   */
    struct task_s  *bucket_next;  /* intrusive link for hash-table bucket      */
} task_t;

/* ── Snapshot (caller-owned copy) ────────────────────────────────────── */
typedef struct {
    char         task_id[37];
    task_state_t state;
    char         username[AUTH_USER_MAX];
    char         method[128];
    char        *result_json;        /* heap copy — caller must free           */
    int          error_code;
    char         error_message[512];
    time_t       created_at, started_at, finished_at, deadline;
    int          timeout_secs;
} task_snapshot_t;

/* ── Init / destroy ──────────────────────────────────────────────────── */
void task_manager_init(void);
void task_manager_destroy(void);

/* ── Create ──────────────────────────────────────────────────────────── */
/*
 * Allocate, register in hash-table, and return a new PENDING task.
 * Caller sets task->fn (and optionally task->fn_arg) before calling
 * worker_pool_submit().
 * task_id_out receives the UUID string (37 bytes); may be NULL.
 */
task_t *task_create(const char *method, const user_info_t *owner,
                    const char *params_json, int timeout_secs,
                    char *task_id_out);

/* ── Direct-pointer transitions (worker thread — no hash lookup) ─────── */
void task_mark_running (task_t *task);
void task_mark_complete(task_t *task, char *result_json); /* takes ownership */
void task_mark_failed  (task_t *task, int err_code, const char *err_msg);

/* ── String-ID API (external JSONRPC calls — hash lookup required) ───── */
bool  task_cancel  (const char *task_id, const char *requester);
bool  task_refresh (const char *task_id, const char *requester);
bool  task_snapshot(const char *task_id, task_snapshot_t *snap);
void  task_snapshot_free(task_snapshot_t *snap);
char *task_list_json(const char *username);   /* caller must free */

/* ── Helpers ─────────────────────────────────────────────────────────── */
const char *task_state_str(task_state_t s);