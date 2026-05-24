#include "methods.h"
#include "task_manager.h"
#include "jsonrpc.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

worker_pool_t *g_worker_pool = NULL;

/* ══ SYNC METHODS ═══════════════════════════════════════════════════════ */

static char *m_ping(const user_info_t *u, yyjson_val *p,
                    int *ec, char *em, size_t esz){
    (void)u;(void)p;(void)ec;(void)em;(void)esz;
    char b[64];snprintf(b,sizeof(b),"{\"pong\":true,\"ts\":%ld}",(long)time(NULL));
    return strdup(b);
}

static char *m_echo(const user_info_t *u, yyjson_val *p,
                    int *ec, char *em, size_t esz){
    (void)ec;(void)em;(void)esz;
    size_t pl; char *pj=yyjson_val_write(p,0,&pl);
    if(!pj)return strdup("{\"echo\":null}");
    int n=snprintf(NULL,0,"{\"echo\":%s,\"user\":\"%s\"}",pj,u->username);
    char *b=malloc(n+1); snprintf(b,n+1,"{\"echo\":%s,\"user\":\"%s\"}",pj,u->username);
    free(pj); return b;
}

static char *m_add(const user_info_t *u, yyjson_val *p,
                   int *ec, char *em, size_t esz){
    (void)u;
    if(!p||!yyjson_is_obj(p)){
        *ec=JRPC_INVALID_PARAMS;snprintf(em,esz,"params must be object");return NULL;}
    yyjson_val *va=yyjson_obj_get(p,"a"), *vb=yyjson_obj_get(p,"b");
    if(!va||!vb||!yyjson_is_num(va)||!yyjson_is_num(vb)){
        *ec=JRPC_INVALID_PARAMS;snprintf(em,esz,"'a' and 'b' must be numbers");return NULL;}
    double a=yyjson_is_int(va)?(double)yyjson_get_sint(va):yyjson_get_real(va);
    double b=yyjson_is_int(vb)?(double)yyjson_get_sint(vb):yyjson_get_real(vb);
    char buf[64]; snprintf(buf,sizeof(buf),"{\"sum\":%g}",a+b);
    return strdup(buf);
}

/* ── task.status ─────────────────────────────────────────────────────── */
static char *m_task_status(const user_info_t *u, yyjson_val *p,
                            int *ec, char *em, size_t esz){
    if(!p){*ec=JRPC_INVALID_PARAMS;snprintf(em,esz,"params required");return NULL;}
    yyjson_val *vt=yyjson_obj_get(p,"task_id");
    if(!vt||!yyjson_is_str(vt)){
        *ec=JRPC_INVALID_PARAMS;snprintf(em,esz,"'task_id' required");return NULL;}
    task_snapshot_t sn;
    if(!task_snapshot(yyjson_get_str(vt),&sn)){
        *ec=JRPC_TASK_NF;snprintf(em,esz,"Task not found: %s",yyjson_get_str(vt));return NULL;}
    if(strcmp(sn.username,u->username)!=0&&strcmp(u->role,"admin")!=0){
        task_snapshot_free(&sn);*ec=JRPC_FORBIDDEN;snprintf(em,esz,"Access denied");return NULL;}
    const char *ss=task_state_str(sn.state);
    time_t now=time(NULL);
    long el=sn.started_at>0?(long)(now-sn.started_at):0;
    long ttl=(sn.deadline>0&&now<sn.deadline)?(long)(sn.deadline-now):0;
    char *buf; int n;
    if(sn.state==TASK_COMPLETED&&sn.result_json){
        n=snprintf(NULL,0,"{\"task_id\":\"%s\",\"state\":\"%s\",\"method\":\"%s\","
            "\"elapsed\":%ld,\"result\":%s}",sn.task_id,ss,sn.method,el,sn.result_json);
        buf=malloc(n+1);
        snprintf(buf,n+1,"{\"task_id\":\"%s\",\"state\":\"%s\",\"method\":\"%s\","
            "\"elapsed\":%ld,\"result\":%s}",sn.task_id,ss,sn.method,el,sn.result_json);
    } else if(sn.state>=TASK_FAILED){
        char safe[512];size_t j=0;
        for(size_t i=0;sn.error_message[i]&&j<sizeof(safe)-2;i++){
            if(sn.error_message[i]=='"')safe[j++]='\\';safe[j++]=sn.error_message[i];}
        safe[j]='\0';
        n=snprintf(NULL,0,"{\"task_id\":\"%s\",\"state\":\"%s\",\"method\":\"%s\","
            "\"error_code\":%d,\"error_message\":\"%s\"}",
            sn.task_id,ss,sn.method,sn.error_code,safe);
        buf=malloc(n+1);
        snprintf(buf,n+1,"{\"task_id\":\"%s\",\"state\":\"%s\",\"method\":\"%s\","
            "\"error_code\":%d,\"error_message\":\"%s\"}",
            sn.task_id,ss,sn.method,sn.error_code,safe);
    } else {
        n=snprintf(NULL,0,"{\"task_id\":\"%s\",\"state\":\"%s\",\"method\":\"%s\","
            "\"elapsed\":%ld,\"ttl\":%ld}",sn.task_id,ss,sn.method,el,ttl);
        buf=malloc(n+1);
        snprintf(buf,n+1,"{\"task_id\":\"%s\",\"state\":\"%s\",\"method\":\"%s\","
            "\"elapsed\":%ld,\"ttl\":%ld}",sn.task_id,ss,sn.method,el,ttl);
    }
    task_snapshot_free(&sn); return buf;
}

static char *m_task_cancel(const user_info_t *u, yyjson_val *p,
                            int *ec, char *em, size_t esz){
    if(!p){*ec=JRPC_INVALID_PARAMS;snprintf(em,esz,"params required");return NULL;}
    yyjson_val *vt=yyjson_obj_get(p,"task_id");
    if(!vt||!yyjson_is_str(vt)){*ec=JRPC_INVALID_PARAMS;snprintf(em,esz,"'task_id' required");return NULL;}
    if(!task_cancel(yyjson_get_str(vt),u->username)){
        *ec=JRPC_TASK_NF;snprintf(em,esz,"Not found or cannot cancel");return NULL;}
    return strdup("{\"cancelled\":true}");
}

static char *m_task_refresh(const user_info_t *u, yyjson_val *p,
                             int *ec, char *em, size_t esz){
    if(!p){*ec=JRPC_INVALID_PARAMS;snprintf(em,esz,"params required");return NULL;}
    yyjson_val *vt=yyjson_obj_get(p,"task_id");
    if(!vt||!yyjson_is_str(vt)){*ec=JRPC_INVALID_PARAMS;snprintf(em,esz,"'task_id' required");return NULL;}
    if(!task_refresh(yyjson_get_str(vt),u->username)){
        *ec=JRPC_TASK_NF;snprintf(em,esz,"Not found, terminal, or access denied");return NULL;}
    return strdup("{\"refreshed\":true}");
}

static char *m_task_list(const user_info_t *u, yyjson_val *p,
                          int *ec, char *em, size_t esz){
    (void)p;(void)ec;(void)em;(void)esz;
    char *arr=task_list_json(u->username);
    if(!arr)return strdup("{\"tasks\":[]}");
    int n=snprintf(NULL,0,"{\"tasks\":%s}",arr);
    char *buf=malloc(n+1); snprintf(buf,n+1,"{\"tasks\":%s}",arr);
    free(arr); return buf;
}

/* ══ ASYNC METHODS ══════════════════════════════════════════════════════
 *
 * Signature: void fn(void *arg)   ← task_work_fn_t = work_fn_t
 * arg is always task_t *; cast at the top.
 * Call task_mark_complete() / task_mark_failed() before returning.
 *
 * ════════════════════════════════════════════════════════════════════════ */

static void slow_compute(void *arg){
    task_t *task = (task_t *)arg;   /* cast: arg is always task_t * */
    LOGI("slow_compute start task=%s params=%s",
         task->task_id, task->params_json ? task->params_json : "{}");

    int n=10;
    if(task->params_json){
        yyjson_doc *d=yyjson_read(task->params_json,strlen(task->params_json),0);
        if(d){
            yyjson_val *vn=yyjson_obj_get(yyjson_doc_get_root(d),"n");
            if(vn&&yyjson_is_int(vn))n=(int)yyjson_get_sint(vn);
            if(n<1)n=1; if(n>40)n=40;
            yyjson_doc_free(d);
        }
    }

    long long a=0,b=1;
    for(int i=0;i<n;i++){
        long long c=a+b; a=b; b=c;
        sleep(1);
        /* Peek at task state — direct pointer read, no hash lookup */
        pthread_mutex_lock(&task->mu);
        bool aborted=(task->state==TASK_CANCELLED||task->state==TASK_TIMED_OUT);
        pthread_mutex_unlock(&task->mu);
        if(aborted){LOGI("slow_compute aborted task=%s",task->task_id);return;}
    }

    char res[128];
    snprintf(res,sizeof(res),"{\"n\":%d,\"fib_n\":%lld}",n,a);
    task_mark_complete(task,strdup(res));   /* direct pointer write */
    LOGI("slow_compute done task=%s fib(%d)=%lld",task->task_id,n,a);
}

/* ══ REGISTRY ═══════════════════════════════════════════════════════════ */

static const method_entry_t g_methods[] = {
    /*  name            async  timeout  sync_fn          async_fn      */
    {  "ping",          false,  0,      m_ping,          NULL          },
    {  "echo",          false,  0,      m_echo,          NULL          },
    {  "add",           false,  0,      m_add,           NULL          },
    {  "slow_compute",  true,   60,     NULL,            slow_compute  },
    {  "task.status",   false,  0,      m_task_status,   NULL          },
    {  "task.cancel",   false,  0,      m_task_cancel,   NULL          },
    {  "task.refresh",  false,  0,      m_task_refresh,  NULL          },
    {  "task.list",     false,  0,      m_task_list,     NULL          },
    {  NULL,            false,  0,      NULL,            NULL          },
};

void methods_init(void){
    LOGI("methods registered: %zu",sizeof(g_methods)/sizeof(g_methods[0])-1);}

const method_entry_t *methods_lookup(const char *name){
    if(!name)return NULL;
    for(int i=0;g_methods[i].name;i++)
        if(strcmp(g_methods[i].name,name)==0)return &g_methods[i];
    return NULL;
}