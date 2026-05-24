#pragma once
#include <pthread.h>
#include <stdbool.h>

/* Universal work unit: fn(arg). All callers use this signature. */
typedef void (*work_fn_t)(void *arg);

typedef struct work_item_s {
    work_fn_t          fn;
    void              *arg;
    struct work_item_s *next;
} work_item_t;

typedef struct {
    pthread_t       *threads;
    int              count;
    work_item_t     *head;
    work_item_t     *tail;
    int              queue_len;
    int              queue_cap;   /* -1 = unlimited */
    pthread_mutex_t  mu;
    pthread_cond_t   cond;
    volatile int     shutdown;
    volatile int     active;
} worker_pool_t;

worker_pool_t *worker_pool_create(int nthreads, int queue_cap);
bool           worker_pool_submit(worker_pool_t *pool, work_fn_t fn, void *arg);
void           worker_pool_destroy(worker_pool_t *pool);
int            worker_pool_active(worker_pool_t *pool);