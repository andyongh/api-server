#include "worker_pool.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

static void *worker_thread(void *arg) {
    worker_pool_t *pool = arg;
    LOGD("worker thread started");

    for (;;) {
        pthread_mutex_lock(&pool->mu);

        while (!pool->queue_head && !pool->shutdown)
            pthread_cond_wait(&pool->cond_work, &pool->mu);

        if (pool->shutdown && !pool->queue_head) {
            pthread_mutex_unlock(&pool->mu);
            break;
        }

        /* Dequeue */
        work_item_t *item = pool->queue_head;
        if (item) {
            pool->queue_head = item->next;
            if (!pool->queue_head) pool->queue_tail = NULL;
            pool->queue_len--;
            pool->active_workers++;
        }
        pthread_mutex_unlock(&pool->mu);

        if (item) {
            item->fn(item->arg);
            free(item);

            pthread_mutex_lock(&pool->mu);
            pool->active_workers--;
            if (pool->active_workers == 0 && !pool->queue_head)
                pthread_cond_broadcast(&pool->cond_drain);
            pthread_mutex_unlock(&pool->mu);
        }
    }

    LOGD("worker thread exiting");
    return NULL;
}

worker_pool_t *worker_pool_create(int thread_count, int queue_cap) {
    worker_pool_t *pool = calloc(1, sizeof(*pool));
    if (!pool) return NULL;

    pool->threads   = calloc(thread_count, sizeof(pthread_t));
    pool->count     = thread_count;
    pool->queue_cap = queue_cap;

    pthread_mutex_init(&pool->mu,         NULL);
    pthread_cond_init (&pool->cond_work,  NULL);
    pthread_cond_init (&pool->cond_drain, NULL);

    for (int i = 0; i < thread_count; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_thread, pool) != 0) {
            LOGE("worker_pool_create: pthread_create failed at i=%d", i);
            pool->count = i;
            break;
        }
    }

    LOGI("worker pool created: %d threads, queue_cap=%d",
         pool->count, queue_cap);
    return pool;
}

bool worker_pool_submit(worker_pool_t *pool, work_fn_t fn, void *arg) {
    if (!pool || !fn) return false;

    pthread_mutex_lock(&pool->mu);

    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->mu);
        LOGW("worker_pool_submit: pool is shutting down, job dropped");
        return false;
    }
    if (pool->queue_cap >= 0 && pool->queue_len >= pool->queue_cap) {
        pthread_mutex_unlock(&pool->mu);
        LOGW("worker_pool_submit: queue full (%d), job dropped", pool->queue_len);
        return false;
    }

    work_item_t *item = malloc(sizeof(*item));
    if (!item) {
        pthread_mutex_unlock(&pool->mu);
        return false;
    }
    item->fn   = fn;
    item->arg  = arg;
    item->next = NULL;

    if (pool->queue_tail)
        pool->queue_tail->next = item;
    else
        pool->queue_head = item;
    pool->queue_tail = item;
    pool->queue_len++;

    pthread_cond_signal(&pool->cond_work);
    pthread_mutex_unlock(&pool->mu);
    return true;
}

void worker_pool_destroy(worker_pool_t *pool) {
    if (!pool) return;

    pthread_mutex_lock(&pool->mu);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->cond_work);
    pthread_mutex_unlock(&pool->mu);

    for (int i = 0; i < pool->count; i++)
        pthread_join(pool->threads[i], NULL);

    /* Drain remaining queue items */
    pthread_mutex_lock(&pool->mu);
    work_item_t *item = pool->queue_head;
    while (item) {
        work_item_t *next = item->next;
        free(item);
        item = next;
    }
    pthread_mutex_unlock(&pool->mu);

    pthread_cond_destroy(&pool->cond_drain);
    pthread_cond_destroy(&pool->cond_work);
    pthread_mutex_destroy(&pool->mu);
    free(pool->threads);
    free(pool);
    LOGI("worker pool destroyed");
}

int worker_pool_active(worker_pool_t *pool) {
    pthread_mutex_lock(&pool->mu);
    int n = pool->active_workers;
    pthread_mutex_unlock(&pool->mu);
    return n;
}
