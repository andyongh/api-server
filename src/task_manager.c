#include "task_manager.h"
#include "uuid.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>

/* ── Internal hash table ─────────────────────────────────────────────── */
#define BUCKET_COUNT   64
#define REAPER_SLEEP_S  2       /* reaper runs every N seconds          */
#define CLEANUP_AGE_S   3600    /* remove terminal tasks older than 1h  */

typedef struct {
    task_t         *head;
    pthread_mutex_t mu;
} bucket_t;

static bucket_t      g_buckets[BUCKET_COUNT];
static pthread_t     g_reaper_tid;
static volatile int  g_shutdown = 0;

/* ── Helpers ──────────────────────────────────────────────────────────── */
static unsigned int bucket_for(const char *task_id) {
    /* FNV-1a on the task_id string */
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)task_id; *p; p++)
        h = (h ^ *p) * 16777619u;
    return h % BUCKET_COUNT;
}

static const char *state_str(task_state_t s) {
    switch (s) {
    case TASK_PENDING:   return "pending";
    case TASK_RUNNING:   return "running";
    case TASK_COMPLETED: return "completed";
    case TASK_FAILED:    return "failed";
    case TASK_CANCELLED: return "cancelled";
    case TASK_TIMED_OUT: return "timed_out";
    default:             return "unknown";
    }
}

/* Find task within a LOCKED bucket — does NOT lock task->mutex. */
static task_t *bucket_find(bucket_t *b, const char *task_id) {
    for (task_t *t = b->head; t; t = t->next)
        if (strcmp(t->task_id, task_id) == 0)
            return t;
    return NULL;
}

/* Remove and free a task from a LOCKED bucket. */
static void __attribute__((unused)) bucket_remove(bucket_t *b, const char *task_id) {
    task_t **pp = &b->head;
    while (*pp) {
        if (strcmp((*pp)->task_id, task_id) == 0) {
            task_t *dead = *pp;
            *pp = dead->next;
            free(dead->params_json);
            free(dead->result_json);
            pthread_mutex_destroy(&dead->mutex);
            free(dead);
            return;
        }
        pp = &(*pp)->next;
    }
}

/* ── Reaper thread ────────────────────────────────────────────────────── */
static void *reaper_thread(void *arg) {
    (void)arg;
    LOGI("task reaper started");

    while (!g_shutdown) {
        sleep(REAPER_SLEEP_S);

        time_t now = time(NULL);
        for (int i = 0; i < BUCKET_COUNT; i++) {
            pthread_mutex_lock(&g_buckets[i].mu);

            task_t *t = g_buckets[i].head;
            while (t) {
                task_t *next = t->next;

                pthread_mutex_lock(&t->mutex);

                /* Expire tasks that exceeded their deadline */
                if ((t->state == TASK_PENDING || t->state == TASK_RUNNING) &&
                     t->deadline > 0 && now > t->deadline) {
                    t->state      = TASK_TIMED_OUT;
                    t->finished_at = now;
                    t->error_code  = -32001;
                    snprintf(t->error_message, sizeof(t->error_message),
                             "Task timed out after %ds", t->timeout_secs);
                    LOGW("task %s timed out (method=%s, user=%s)",
                         t->task_id, t->method, t->owner.username);
                }

                /* Collect terminal tasks older than CLEANUP_AGE_S */
                bool stale = (t->state >= TASK_COMPLETED) &&
                             (now - t->finished_at > CLEANUP_AGE_S);
                pthread_mutex_unlock(&t->mutex);

                if (stale) {
                    LOGD("task reaper removing stale task %s", t->task_id);
                    /* We hold bucket lock, safe to remove */
                    task_t **pp = &g_buckets[i].head;
                    while (*pp && *pp != t) pp = &(*pp)->next;
                    if (*pp == t) {
                        *pp = t->next;
                        free(t->params_json);
                        free(t->result_json);
                        pthread_mutex_destroy(&t->mutex);
                        free(t);
                    }
                }
                t = next;
            }

            pthread_mutex_unlock(&g_buckets[i].mu);
        }
    }

    LOGI("task reaper stopped");
    return NULL;
}

/* ── Public API ───────────────────────────────────────────────────────── */
void task_manager_init(void) {
    for (int i = 0; i < BUCKET_COUNT; i++) {
        g_buckets[i].head = NULL;
        pthread_mutex_init(&g_buckets[i].mu, NULL);
    }
    g_shutdown = 0;
    pthread_create(&g_reaper_tid, NULL, reaper_thread, NULL);
    LOGI("task manager initialised (%d buckets)", BUCKET_COUNT);
}

void task_manager_destroy(void) {
    g_shutdown = 1;
    pthread_join(g_reaper_tid, NULL);

    for (int i = 0; i < BUCKET_COUNT; i++) {
        pthread_mutex_lock(&g_buckets[i].mu);
        task_t *t = g_buckets[i].head;
        while (t) {
            task_t *next = t->next;
            free(t->params_json);
            free(t->result_json);
            pthread_mutex_destroy(&t->mutex);
            free(t);
            t = next;
        }
        g_buckets[i].head = NULL;
        pthread_mutex_unlock(&g_buckets[i].mu);
        pthread_mutex_destroy(&g_buckets[i].mu);
    }
    LOGI("task manager destroyed");
}

task_t *task_create(const char *method, const user_info_t *owner,
                    const char *params_json, int timeout_secs,
                    char *task_id_out) {
    task_t *t = calloc(1, sizeof(*t));
    if (!t) return NULL;

    uuid_v4(t->task_id);
    t->state       = TASK_PENDING;
    t->owner       = *owner;
    t->created_at  = time(NULL);
    t->timeout_secs = timeout_secs;
    t->deadline    = (timeout_secs > 0) ? (t->created_at + timeout_secs) : 0;

    snprintf(t->method, sizeof(t->method), "%s", method);
    if (params_json)
        t->params_json = strdup(params_json);

    pthread_mutex_init(&t->mutex, NULL);

    unsigned int bi = bucket_for(t->task_id);
    pthread_mutex_lock(&g_buckets[bi].mu);
    t->next = g_buckets[bi].head;
    g_buckets[bi].head = t;
    pthread_mutex_unlock(&g_buckets[bi].mu);

    if (task_id_out)
        memcpy(task_id_out, t->task_id, 37);

    LOGI("task created id=%s method=%s user=%s timeout=%ds",
         t->task_id, t->method, t->owner.username, t->timeout_secs);
    return t;
}

bool task_set_running(const char *task_id) {
    unsigned int bi = bucket_for(task_id);
    pthread_mutex_lock(&g_buckets[bi].mu);
    task_t *t = bucket_find(&g_buckets[bi], task_id);
    if (!t) { pthread_mutex_unlock(&g_buckets[bi].mu); return false; }

    pthread_mutex_lock(&t->mutex);
    if (t->state == TASK_PENDING) {
        t->state      = TASK_RUNNING;
        t->started_at = time(NULL);
    }
    pthread_mutex_unlock(&t->mutex);
    pthread_mutex_unlock(&g_buckets[bi].mu);
    return true;
}

bool task_complete(const char *task_id, char *result_json) {
    unsigned int bi = bucket_for(task_id);
    pthread_mutex_lock(&g_buckets[bi].mu);
    task_t *t = bucket_find(&g_buckets[bi], task_id);
    if (!t) { pthread_mutex_unlock(&g_buckets[bi].mu); free(result_json); return false; }

    pthread_mutex_lock(&t->mutex);
    if (t->state == TASK_RUNNING || t->state == TASK_PENDING) {
        t->state       = TASK_COMPLETED;
        t->finished_at = time(NULL);
        free(t->result_json);
        t->result_json = result_json;   /* takes ownership */
        LOGI("task completed id=%s", task_id);
    } else {
        free(result_json);              /* discard if already cancelled/timed-out */
    }
    pthread_mutex_unlock(&t->mutex);
    pthread_mutex_unlock(&g_buckets[bi].mu);
    return true;
}

bool task_fail(const char *task_id, int err_code, const char *err_msg) {
    unsigned int bi = bucket_for(task_id);
    pthread_mutex_lock(&g_buckets[bi].mu);
    task_t *t = bucket_find(&g_buckets[bi], task_id);
    if (!t) { pthread_mutex_unlock(&g_buckets[bi].mu); return false; }

    pthread_mutex_lock(&t->mutex);
    if (t->state == TASK_RUNNING || t->state == TASK_PENDING) {
        t->state       = TASK_FAILED;
        t->finished_at = time(NULL);
        t->error_code  = err_code;
        if (err_msg)
            snprintf(t->error_message, sizeof(t->error_message), "%s", err_msg);
        LOGW("task failed id=%s code=%d msg=%s", task_id, err_code, err_msg);
    }
    pthread_mutex_unlock(&t->mutex);
    pthread_mutex_unlock(&g_buckets[bi].mu);
    return true;
}

bool task_cancel(const char *task_id, const char *requester) {
    unsigned int bi = bucket_for(task_id);
    pthread_mutex_lock(&g_buckets[bi].mu);
    task_t *t = bucket_find(&g_buckets[bi], task_id);
    if (!t) { pthread_mutex_unlock(&g_buckets[bi].mu); return false; }

    bool ok = false;
    pthread_mutex_lock(&t->mutex);
    /* Only owner or admin may cancel */
    bool may = (strcmp(t->owner.username, requester) == 0) ||
               /* crude role check — in production use user_info_t role */
               (strncmp(requester, "admin", 5) == 0);
    if (may && (t->state == TASK_PENDING || t->state == TASK_RUNNING)) {
        t->state       = TASK_CANCELLED;
        t->finished_at = time(NULL);
        snprintf(t->error_message, sizeof(t->error_message),
                 "Cancelled by %s", requester);
        LOGI("task cancelled id=%s by=%s", task_id, requester);
        ok = true;
    }
    pthread_mutex_unlock(&t->mutex);
    pthread_mutex_unlock(&g_buckets[bi].mu);
    return ok;
}

bool task_refresh(const char *task_id, const char *requester) {
    unsigned int bi = bucket_for(task_id);
    pthread_mutex_lock(&g_buckets[bi].mu);
    task_t *t = bucket_find(&g_buckets[bi], task_id);
    if (!t) { pthread_mutex_unlock(&g_buckets[bi].mu); return false; }

    bool ok = false;
    pthread_mutex_lock(&t->mutex);
    if (strcmp(t->owner.username, requester) == 0 &&
        t->state <= TASK_RUNNING && t->timeout_secs > 0) {
        t->deadline = time(NULL) + t->timeout_secs;
        LOGD("task %s deadline refreshed (+%ds)", task_id, t->timeout_secs);
        ok = true;
    }
    pthread_mutex_unlock(&t->mutex);
    pthread_mutex_unlock(&g_buckets[bi].mu);
    return ok;
}

bool task_snapshot(const char *task_id, task_snapshot_t *snap) {
    unsigned int bi = bucket_for(task_id);
    pthread_mutex_lock(&g_buckets[bi].mu);
    task_t *t = bucket_find(&g_buckets[bi], task_id);
    if (!t) { pthread_mutex_unlock(&g_buckets[bi].mu); return false; }

    pthread_mutex_lock(&t->mutex);
    memcpy(snap->task_id,       t->task_id,            37);
    snap->state = t->state;
    snprintf(snap->username,    sizeof(snap->username), "%s", t->owner.username);
    snprintf(snap->method,      sizeof(snap->method),   "%s", t->method);
    snap->result_json  = t->result_json ? strdup(t->result_json) : NULL;
    snap->error_code   = t->error_code;
    snprintf(snap->error_message, sizeof(snap->error_message),
             "%s", t->error_message);
    snap->created_at   = t->created_at;
    snap->started_at   = t->started_at;
    snap->finished_at  = t->finished_at;
    snap->deadline     = t->deadline;
    snap->timeout_secs = t->timeout_secs;
    pthread_mutex_unlock(&t->mutex);
    pthread_mutex_unlock(&g_buckets[bi].mu);
    return true;
}

void task_snapshot_free(task_snapshot_t *snap) {
    if (snap) { free(snap->result_json); snap->result_json = NULL; }
}

char *task_list_json(const char *username) {
    /* Build a JSON array of task snapshots owned by username */
    char  *buf  = NULL;
    size_t sz   = 0;
    size_t cap  = 4096;
    buf = malloc(cap);
    if (!buf) return NULL;

    sz += (size_t)snprintf(buf + sz, cap - sz, "[");

    bool first = true;
    time_t now = time(NULL);

    for (int i = 0; i < BUCKET_COUNT; i++) {
        pthread_mutex_lock(&g_buckets[i].mu);
        for (task_t *t = g_buckets[i].head; t; t = t->next) {
            pthread_mutex_lock(&t->mutex);
            bool match = (strcmp(t->owner.username, username) == 0);
            if (match) {
                long elapsed = (t->started_at > 0) ? (long)(now - t->started_at) : 0;
                long ttl     = (t->deadline > 0 && now < t->deadline)
                               ? (long)(t->deadline - now) : 0;

                /* Grow buffer if needed */
                if (cap - sz < 512) {
                    cap *= 2;
                    char *nb = realloc(buf, cap);
                    if (!nb) { pthread_mutex_unlock(&t->mutex); pthread_mutex_unlock(&g_buckets[i].mu); goto done; }
                    buf = nb;
                }
                sz += (size_t)snprintf(buf + sz, cap - sz,
                    "%s{\"task_id\":\"%s\",\"method\":\"%s\","
                    "\"state\":\"%s\",\"elapsed\":%ld,\"ttl\":%ld}",
                    first ? "" : ",",
                    t->task_id, t->method, state_str(t->state), elapsed, ttl);
                first = false;
            }
            pthread_mutex_unlock(&t->mutex);
        }
        pthread_mutex_unlock(&g_buckets[i].mu);
    }

done:
    if (cap - sz < 4) {
        cap += 4;
        buf = realloc(buf, cap);
    }
    sz += (size_t)snprintf(buf + sz, cap - sz, "]");
    return buf;
}
