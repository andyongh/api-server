#pragma once
#include "auth.h"
#include <time.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

/* ── Task states ──────────────────────────────────────────────────────── */
typedef enum {
    TASK_PENDING   = 0,  /* queued, not yet picked up by worker      */
    TASK_RUNNING   = 1,  /* worker is processing it                  */
    TASK_COMPLETED = 2,  /* finished successfully                    */
    TASK_FAILED    = 3,  /* finished with an application-level error */
    TASK_CANCELLED = 4,  /* cancelled by client                      */
    TASK_TIMED_OUT = 5,  /* deadline exceeded                        */
} task_state_t;

/* ── Task record ──────────────────────────────────────────────────────── */
typedef struct task_s {
    char           task_id[37];           /* UUID string (null-terminated) */
    task_state_t   state;
    user_info_t    owner;

    char           method[128];
    char          *params_json;           /* heap — owned, may be NULL     */

    char          *result_json;           /* heap — set on COMPLETED       */
    int            error_code;            /* set on FAILED / TIMED_OUT     */
    char           error_message[512];

    time_t         created_at;
    time_t         started_at;
    time_t         finished_at;
    time_t         deadline;              /* epoch; 0 = no timeout         */
    int            timeout_secs;          /* original timeout value        */

    pthread_mutex_t mutex;                /* protects mutable fields       */
    struct task_s  *next;                 /* hash-bucket linked list       */
} task_t;

/* ── Snapshot (lock-free copy for callers) ────────────────────────────── */
typedef struct {
    char         task_id[37];
    task_state_t state;
    char         username[AUTH_USER_MAX];
    char         method[128];
    char        *result_json;   /* heap copy — caller must free          */
    int          error_code;
    char         error_message[512];
    time_t       created_at;
    time_t       started_at;
    time_t       finished_at;
    time_t       deadline;
    int          timeout_secs;
} task_snapshot_t;

/* ── Lifecycle ────────────────────────────────────────────────────────── */

/** Initialise the task manager (call once at startup). */
void task_manager_init(void);

/** Shut down: stop the reaper thread, destroy all tasks. */
void task_manager_destroy(void);

/**
 * Create a new task. Fills task_id_out (37 bytes) with the assigned UUID.
 * @return pointer to the (locked-into-table) task, or NULL on OOM.
 */
task_t *task_create(const char *method,
                    const user_info_t *owner,
                    const char *params_json,
                    int timeout_secs,
                    char *task_id_out);

/** Mark task as RUNNING (sets started_at). */
bool task_set_running(const char *task_id);

/** Mark task as COMPLETED, store a heap-allocated result_json (takes ownership). */
bool task_complete(const char *task_id, char *result_json);

/** Mark task as FAILED with an error code and message. */
bool task_fail(const char *task_id, int err_code, const char *err_msg);

/** Mark task as CANCELLED (only if PENDING or RUNNING). */
bool task_cancel(const char *task_id, const char *requester_username);

/**
 * Refresh the deadline by timeout_secs from now.
 * Returns false if task does not exist or is already terminal.
 */
bool task_refresh(const char *task_id, const char *requester_username);

/**
 * Get a snapshot of the task.
 * Caller must free snap->result_json when done.
 * Returns false if task not found.
 */
bool task_snapshot(const char *task_id, task_snapshot_t *snap);

/** Free fields inside a snapshot (result_json). */
void task_snapshot_free(task_snapshot_t *snap);

/**
 * List all tasks owned by username into a newly allocated JSON array string.
 * Caller must free the returned string.
 */
char *task_list_json(const char *username);