#include "worker_pool.h"
#include "log.h"
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>

static void *thread_fn(void *arg){
    worker_pool_t *p=arg;
    LOGD("worker started");
    for(;;){
        pthread_mutex_lock(&p->mu);
        while(!p->head && !p->shutdown)
            pthread_cond_wait(&p->cond,&p->mu);
        if(p->shutdown && !p->head){pthread_mutex_unlock(&p->mu);break;}
        work_item_t *it=p->head;
        if(it){p->head=it->next;if(!p->head)p->tail=NULL;p->queue_len--;p->active++;}
        pthread_mutex_unlock(&p->mu);
        if(it){it->fn(it->arg);free(it);
               pthread_mutex_lock(&p->mu);p->active--;pthread_mutex_unlock(&p->mu);}
    }
    LOGD("worker exiting");
    return NULL;
}

worker_pool_t *worker_pool_create(int n, int cap){
    worker_pool_t *p=calloc(1,sizeof(*p));
    if(!p)return NULL;
    p->threads=calloc(n,sizeof(pthread_t));
    p->count=n; p->queue_cap=cap;
    pthread_mutex_init(&p->mu,NULL);
    pthread_cond_init(&p->cond,NULL);
    for(int i=0;i<n;i++){
        if(pthread_create(&p->threads[i],NULL,thread_fn,p)!=0)
            {p->count=i;break;}
    }
    LOGI("worker pool: %d threads cap=%d",p->count,cap);
    return p;
}

bool worker_pool_submit(worker_pool_t *p, work_fn_t fn, void *arg){
    if(!p||!fn)return false;
    pthread_mutex_lock(&p->mu);
    if(p->shutdown||
       (p->queue_cap>=0 && p->queue_len>=p->queue_cap)){
        pthread_mutex_unlock(&p->mu);
        LOGW("pool submit rejected (shutdown=%d qlen=%d)",p->shutdown,p->queue_len);
        return false;
    }
    work_item_t *it=malloc(sizeof(*it));
    if(!it){pthread_mutex_unlock(&p->mu);return false;}
    it->fn=fn; it->arg=arg; it->next=NULL;
    if(p->tail)p->tail->next=it; else p->head=it;
    p->tail=it; p->queue_len++;
    pthread_cond_signal(&p->cond);
    pthread_mutex_unlock(&p->mu);
    return true;
}

void worker_pool_destroy(worker_pool_t *p){
    if(!p)return;
    pthread_mutex_lock(&p->mu);
    p->shutdown=1;
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->mu);
    for(int i=0;i<p->count;i++)pthread_join(p->threads[i],NULL);
    work_item_t *it=p->head;
    while(it){work_item_t *nx=it->next;free(it);it=nx;}
    pthread_cond_destroy(&p->cond);
    pthread_mutex_destroy(&p->mu);
    free(p->threads); free(p);
    LOGI("worker pool destroyed");
}

int worker_pool_active(worker_pool_t *p){
    pthread_mutex_lock(&p->mu);int n=p->active;pthread_mutex_unlock(&p->mu);return n;
}