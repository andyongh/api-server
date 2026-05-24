#include "task_manager.h"
#include "uuid.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#define BUCKETS        64
#define REAPER_SLEEP    2
#define CLEANUP_AGE  3600

typedef struct { task_t *head; pthread_mutex_t mu; } bucket_t;
static bucket_t     g_b[BUCKETS];
static pthread_t    g_reaper;
static volatile int g_stop = 0;

static unsigned int bkt(const char *id){
    uint32_t h=2166136261u;
    for(const unsigned char *p=(const unsigned char*)id;*p;p++)h=(h^*p)*16777619u;
    return h%BUCKETS;
}

const char *task_state_str(task_state_t s){
    switch(s){case TASK_PENDING:return"pending";case TASK_RUNNING:return"running";
              case TASK_COMPLETED:return"completed";case TASK_FAILED:return"failed";
              case TASK_CANCELLED:return"cancelled";case TASK_TIMED_OUT:return"timed_out";
              default:return"unknown";}
}

static task_t *find_locked(bucket_t *b, const char *id){
    for(task_t *t=b->head;t;t=t->bucket_next)
        if(strcmp(t->task_id,id)==0)return t;
    return NULL;
}

static void task_free(task_t *t){
    free(t->params_json); free(t->result_json);
    pthread_mutex_destroy(&t->mu); free(t);
}

static void *reaper_fn(void *arg){
    (void)arg; LOGI("reaper started");
    while(!g_stop){
        sleep(REAPER_SLEEP);
        time_t now=time(NULL);
        for(int i=0;i<BUCKETS;i++){
            pthread_mutex_lock(&g_b[i].mu);
            task_t **pp=&g_b[i].head;
            while(*pp){
                task_t *t=*pp;
                pthread_mutex_lock(&t->mu);
                if((t->state==TASK_PENDING||t->state==TASK_RUNNING)
                   && t->deadline>0 && now>t->deadline){
                    t->state=TASK_TIMED_OUT; t->finished_at=now;
                    t->error_code=-32002;
                    snprintf(t->error_message,sizeof(t->error_message),
                             "Timed out after %ds",t->timeout_secs);
                    LOGW("task %s timed out",t->task_id);
                }
                bool stale=(t->state>=TASK_COMPLETED)&&
                           (now-t->finished_at>CLEANUP_AGE);
                pthread_mutex_unlock(&t->mu);
                if(stale){LOGD("reaper removes %s",t->task_id);
                           *pp=t->bucket_next;task_free(t);}
                else pp=&t->bucket_next;
            }
            pthread_mutex_unlock(&g_b[i].mu);
        }
    }
    LOGI("reaper stopped"); return NULL;
}

void task_manager_init(void){
    for(int i=0;i<BUCKETS;i++){g_b[i].head=NULL;pthread_mutex_init(&g_b[i].mu,NULL);}
    g_stop=0; pthread_create(&g_reaper,NULL,reaper_fn,NULL);
    LOGI("task manager init (%d buckets)",BUCKETS);
}
void task_manager_destroy(void){
    g_stop=1; pthread_join(g_reaper,NULL);
    for(int i=0;i<BUCKETS;i++){
        pthread_mutex_lock(&g_b[i].mu);
        task_t *t=g_b[i].head;
        while(t){task_t *nx=t->bucket_next;task_free(t);t=nx;}
        g_b[i].head=NULL;
        pthread_mutex_unlock(&g_b[i].mu);
        pthread_mutex_destroy(&g_b[i].mu);
    }
    LOGI("task manager destroyed");
}

task_t *task_create(const char *method, const user_info_t *owner,
                    const char *params_json, int timeout_secs,
                    char *id_out){
    task_t *t=calloc(1,sizeof(*t));
    if(!t)return NULL;
    uuid_v4(t->task_id);
    t->state=TASK_PENDING; t->owner=*owner;
    t->created_at=time(NULL);
    t->timeout_secs=timeout_secs;
    t->deadline=(timeout_secs>0)?(t->created_at+timeout_secs):0;
    snprintf(t->method,sizeof(t->method),"%s",method);
    if(params_json)t->params_json=strdup(params_json);
    pthread_mutex_init(&t->mu,NULL);
    unsigned int bi=bkt(t->task_id);
    pthread_mutex_lock(&g_b[bi].mu);
    t->bucket_next=g_b[bi].head; g_b[bi].head=t;
    pthread_mutex_unlock(&g_b[bi].mu);
    if(id_out)memcpy(id_out,t->task_id,37);
    LOGI("task created id=%s method=%s user=%s timeout=%ds",
         t->task_id,t->method,t->owner.username,t->timeout_secs);
    return t;
}

/* ── Direct-pointer transitions ───────────────────────────────────────── */
void task_mark_running(task_t *t){
    pthread_mutex_lock(&t->mu);
    if(t->state==TASK_PENDING){t->state=TASK_RUNNING;t->started_at=time(NULL);}
    pthread_mutex_unlock(&t->mu);
}
void task_mark_complete(task_t *t, char *result_json){
    pthread_mutex_lock(&t->mu);
    if(t->state==TASK_RUNNING||t->state==TASK_PENDING){
        t->state=TASK_COMPLETED; t->finished_at=time(NULL);
        free(t->result_json); t->result_json=result_json;
        LOGI("task completed id=%s",t->task_id);
    } else {
        LOGD("task %s already terminal (%s), discard result",
             t->task_id,task_state_str(t->state));
        free(result_json);
    }
    pthread_mutex_unlock(&t->mu);
}
void task_mark_failed(task_t *t, int code, const char *msg){
    pthread_mutex_lock(&t->mu);
    if(t->state==TASK_RUNNING||t->state==TASK_PENDING){
        t->state=TASK_FAILED; t->finished_at=time(NULL);
        t->error_code=code;
        if(msg)snprintf(t->error_message,sizeof(t->error_message),"%s",msg);
        LOGW("task failed id=%s code=%d",t->task_id,code);
    }
    pthread_mutex_unlock(&t->mu);
}

/* ── String-ID API ───────────────────────────────────────────────────── */
bool task_cancel(const char *id, const char *who){
    unsigned int bi=bkt(id);
    pthread_mutex_lock(&g_b[bi].mu);
    task_t *t=find_locked(&g_b[bi],id);
    if(!t){pthread_mutex_unlock(&g_b[bi].mu);return false;}
    bool ok=false;
    pthread_mutex_lock(&t->mu);
    bool may=(strcmp(t->owner.username,who)==0)||(strncmp(who,"admin",5)==0);
    if(may && t->state<TASK_COMPLETED){
        t->state=TASK_CANCELLED; t->finished_at=time(NULL);
        snprintf(t->error_message,sizeof(t->error_message),"Cancelled by %s",who);
        LOGI("task cancelled id=%s by=%s",id,who); ok=true;
    }
    pthread_mutex_unlock(&t->mu);
    pthread_mutex_unlock(&g_b[bi].mu);
    return ok;
}
bool task_refresh(const char *id, const char *who){
    unsigned int bi=bkt(id);
    pthread_mutex_lock(&g_b[bi].mu);
    task_t *t=find_locked(&g_b[bi],id);
    if(!t){pthread_mutex_unlock(&g_b[bi].mu);return false;}
    bool ok=false;
    pthread_mutex_lock(&t->mu);
    if(strcmp(t->owner.username,who)==0 && t->state<TASK_COMPLETED && t->timeout_secs>0){
        t->deadline=time(NULL)+t->timeout_secs;
        LOGD("task %s deadline refreshed +%ds",id,t->timeout_secs); ok=true;
    }
    pthread_mutex_unlock(&t->mu);
    pthread_mutex_unlock(&g_b[bi].mu);
    return ok;
}
bool task_snapshot(const char *id, task_snapshot_t *snap){
    unsigned int bi=bkt(id);
    pthread_mutex_lock(&g_b[bi].mu);
    task_t *t=find_locked(&g_b[bi],id);
    if(!t){pthread_mutex_unlock(&g_b[bi].mu);return false;}
    pthread_mutex_lock(&t->mu);
    memcpy(snap->task_id,t->task_id,37);
    snap->state=t->state;
    snprintf(snap->username,sizeof(snap->username),"%s",t->owner.username);
    snprintf(snap->method,sizeof(snap->method),"%s",t->method);
    snap->result_json=t->result_json?strdup(t->result_json):NULL;
    snap->error_code=t->error_code;
    snprintf(snap->error_message,sizeof(snap->error_message),"%s",t->error_message);
    snap->created_at=t->created_at; snap->started_at=t->started_at;
    snap->finished_at=t->finished_at; snap->deadline=t->deadline;
    snap->timeout_secs=t->timeout_secs;
    pthread_mutex_unlock(&t->mu);
    pthread_mutex_unlock(&g_b[bi].mu);
    return true;
}
void task_snapshot_free(task_snapshot_t *s){if(s){free(s->result_json);s->result_json=NULL;}}

char *task_list_json(const char *username){
    size_t sz=0,cap=4096;
    char *buf=malloc(cap);
    if(!buf)return NULL;
    sz+=(size_t)snprintf(buf,cap,"[");
    bool first=true; time_t now=time(NULL);
    for(int i=0;i<BUCKETS;i++){
        pthread_mutex_lock(&g_b[i].mu);
        for(task_t *t=g_b[i].head;t;t=t->bucket_next){
            pthread_mutex_lock(&t->mu);
            if(strcmp(t->owner.username,username)==0){
                long el=(t->started_at>0)?(long)(now-t->started_at):0;
                long ttl=(t->deadline>0&&now<t->deadline)?(long)(t->deadline-now):0;
                if(cap-sz<512){cap*=2;char*nb=realloc(buf,cap);
                    if(!nb){pthread_mutex_unlock(&t->mu);pthread_mutex_unlock(&g_b[i].mu);goto done;}
                    buf=nb;}
                sz+=(size_t)snprintf(buf+sz,cap-sz,
                    "%s{\"task_id\":\"%s\",\"method\":\"%s\","
                    "\"state\":\"%s\",\"elapsed\":%ld,\"ttl\":%ld}",
                    first?"":",",t->task_id,t->method,
                    task_state_str(t->state),el,ttl);
                first=false;
            }
            pthread_mutex_unlock(&t->mu);
        }
        pthread_mutex_unlock(&g_b[i].mu);
    }
done:
    if(cap-sz<4){cap+=4;buf=realloc(buf,cap);}
    sz+=(size_t)snprintf(buf+sz,cap-sz,"]");
    return buf;
}