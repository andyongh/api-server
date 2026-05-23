#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

typedef void (*work_fn_t)(void *arg);

typedef struct work_item_s {
    work_fn_t         fn;
    void             *arg;
    struct work_item_s *next;
} work_item_t;

typedef struct {
    pthread_t       *threads;
    int              count;

    work_item_t     *queue_head;
    work_item_t     *queue_tail;
    int              queue_len;
    int              queue_cap;      /* -1 = unlimited */

    pthread_mutex_t  mu;
    pthread_cond_t   cond_work;
    pthread_cond_t   cond_drain;
    volatile int     shutdown;
    volatile int     active_workers; /* tasks currently running */
} worker_pool_t;

/**
 * Create a worker pool with `thread_count` threads.
 * `queue_cap` sets the maximum pending-job queue depth (-1 = unlimited).
 */
worker_pool_t *worker_pool_create(int thread_count, int queue_cap);

/**
 * Submit a job.  Returns false if the queue is full or pool is shutting down.
 */
bool worker_pool_submit(worker_pool_t *pool, work_fn_t fn, void *arg);

/**
 * Signal shutdown and wait for all threads to finish in-progress work.
 * Does NOT drain the queue — pending jobs are dropped.
 */
void worker_pool_destroy(worker_pool_t *pool);

/** Return number of currently active (executing) workers. */
int  worker_pool_active(worker_pool_t *pool);