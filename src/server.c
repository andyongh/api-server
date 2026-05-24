#include "server.h"
#include "jsonrpc.h"
#include "auth.h"
#include "methods.h"
#include "task_manager.h"
#include "log.h"
#include <microhttpd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Per-connection state ─────────────────────────────────────────────── */
typedef enum { CI_READING=0, CI_PROCESSING=1, CI_READY=2 } ci_state_t;

typedef struct {
    volatile ci_state_t state;
    struct MHD_Connection *mhd_conn;
    char   *req;  size_t req_len; size_t req_cap;
    bool    too_large;
    char   *resp; int    http_status;
} conn_info_t;

/* ── Sync dispatch context ────────────────────────────────────────────── */
typedef struct {
    conn_info_t          *ci;
    jrpc_req_t           *req;
    const method_entry_t *entry;
} sync_ctx_t;

/* ── task_runner ──────────────────────────────────────────────────────────
 *
 * Submitted to the worker pool for every async task.
 * Signature is work_fn_t = void(*)(void*), which equals task_work_fn_t.
 *
 * Flow:
 *   task->fn is set to entry->async_fn before submission.
 *   Both task->fn and entry->async_fn are void(*)(void*).
 *   No cast needed when assigning.  arg → task_t* cast happens here
 *   and inside the actual business function.
 */
static void task_runner(void *arg){
    task_t *task=(task_t *)arg;
    task_mark_running(task);        /* PENDING→RUNNING via direct pointer   */
    task->fn((void *)task);         /* business fn: void(*)(void*)          */
}

/* ── do_sync ──────────────────────────────────────────────────────────── */
static void do_sync(void *arg){
    sync_ctx_t *ctx=(sync_ctx_t *)arg;
    conn_info_t *ci=ctx->ci;
    jrpc_req_t  *req=ctx->req;
    const method_entry_t *entry=ctx->entry;
    free(ctx);

    int  ec=0; char em[512]={0};
    char *result=entry->sync_fn(&req->user,req->params,&ec,em,sizeof(em));
    char *body=result ? jrpc_result(req->id_str,result)
                      : jrpc_error (req->id_str,ec,em);
    free(result);
    jrpc_req_free(req);

    ci->resp=body;
    ci->http_status=200;
    __atomic_store_n((int*)&ci->state,CI_READY,__ATOMIC_SEQ_CST);
    MHD_resume_connection(ci->mhd_conn);
    LOGD("sync done, connection resumed");
}

/* ── Helpers ──────────────────────────────────────────────────────────── */
static void add_headers(struct MHD_Response *r){
    MHD_add_response_header(r,"Content-Type","application/json");
    MHD_add_response_header(r,"Access-Control-Allow-Origin","*");
}

static enum MHD_Result send_json(struct MHD_Connection *c,int status,char *body){
    struct MHD_Response *r=MHD_create_response_from_buffer(
        strlen(body),body,MHD_RESPMEM_MUST_FREE);
    if(!r){free(body);return MHD_NO;}
    add_headers(r);
    enum MHD_Result ret=MHD_queue_response(c,(unsigned int)status,r);
    MHD_destroy_response(r);
    return ret;
}

/* ── Request handler ──────────────────────────────────────────────────── */
static enum MHD_Result handle(void *cls,
    struct MHD_Connection *conn, const char *url, const char *method,
    const char *version, const char *upload_data,
    size_t *upload_data_size, void **con_cls)
{
    (void)url;(void)version;
    worker_pool_t *pool=(worker_pool_t *)cls;

    /* First call */
    if(!*con_cls){
        if(strcmp(method,"OPTIONS")==0){
            struct MHD_Response *r=MHD_create_response_from_buffer(0,"",MHD_RESPMEM_PERSISTENT);
            add_headers(r);
            MHD_add_response_header(r,"Access-Control-Allow-Methods","POST, OPTIONS");
            enum MHD_Result ret=MHD_queue_response(conn,MHD_HTTP_OK,r);
            MHD_destroy_response(r);return ret;
        }
        if(strcmp(method,"POST")!=0)
            return send_json(conn,MHD_HTTP_METHOD_NOT_ALLOWED,
                jrpc_error("null",JRPC_INVALID_REQ,"Only POST accepted"));
        conn_info_t *ci=calloc(1,sizeof(*ci));
        if(!ci)return MHD_NO;
        ci->state=CI_READING; ci->mhd_conn=conn; ci->http_status=200;
        *con_cls=ci; return MHD_YES;
    }

    conn_info_t *ci=*con_cls;

    /* Accumulate body */
    if(*upload_data_size>0){
        if(ci->req_len+*upload_data_size>MAX_BODY_SIZE){
            ci->too_large=true; *upload_data_size=0; return MHD_YES;}
        size_t nl=ci->req_len+*upload_data_size;
        if(nl+1>ci->req_cap){
            size_t nc=(nl+1)*2;
            char *nb=realloc(ci->req,nc);if(!nb)return MHD_NO;
            ci->req=nb;ci->req_cap=nc;}
        memcpy(ci->req+ci->req_len,upload_data,*upload_data_size);
        ci->req_len+=*upload_data_size;
        ci->req[ci->req_len]='\0';
        *upload_data_size=0; return MHD_YES;
    }

    /* Body complete — first time */
    if(ci->state==CI_READING){
        if(ci->too_large)
            return send_json(conn,MHD_HTTP_CONTENT_TOO_LARGE,
                jrpc_error("null",JRPC_INVALID_REQ,"Body too large"));
        if(!ci->req||ci->req_len==0)
            return send_json(conn,MHD_HTTP_BAD_REQUEST,
                jrpc_error("null",JRPC_PARSE,"Empty body"));

        /* Parse */
        char *perr=NULL;
        jrpc_req_t *req=jrpc_parse(ci->req,ci->req_len,&perr);
        if(!req)return send_json(conn,MHD_HTTP_BAD_REQUEST,perr);

        /* Auth */
        if(!auth_verify(req->session,&req->user)){
            LOGW("auth failed session=%.32s",req->session);
            char *e=jrpc_error(req->id_str,JRPC_AUTH,"Authentication failed");
            jrpc_req_free(req);
            return send_json(conn,MHD_HTTP_UNAUTHORIZED,e);
        }
        LOGD("auth ok user=%s method=%s",req->user.username,req->method);

        /* Lookup */
        const method_entry_t *entry=methods_lookup(req->method);
        if(!entry){
            char em[160];
            snprintf(em,sizeof(em),"Method not found: %s",req->method);
            char *e=jrpc_error(req->id_str,JRPC_METHOD_NF,em);
            jrpc_req_free(req);
            return send_json(conn,MHD_HTTP_OK,e);
        }

        /* ── ASYNC ──────────────────────────────────────────────────── */
        if(entry->is_async){
            task_t *task=task_create(req->method,&req->user,
                                      req->params_json,entry->timeout_secs,NULL);
            if(!task){
                char *e=jrpc_error(req->id_str,JRPC_INTERNAL,"OOM");
                jrpc_req_free(req); return send_json(conn,500,e);
            }
            /*
             * task->fn and entry->async_fn are both task_work_fn_t
             * = work_fn_t = void(*)(void *).
             * Direct assignment — no cast, no wrapper.
             */
            task->fn = entry->async_fn;

            if(!worker_pool_submit(pool,task_runner,(void *)task))
                task_mark_failed(task,JRPC_QUEUE_FULL,"Worker queue full");

            LOGI("async queued id=%s method=%s user=%s",
                 task->task_id,req->method,req->user.username);
            char *acc=jrpc_accepted(req->id_str,task->task_id,entry->timeout_secs);
            jrpc_req_free(req);
            return send_json(conn,MHD_HTTP_ACCEPTED,acc);
        }

        /* ── SYNC: suspend → worker → resume ────────────────────────── */
        sync_ctx_t *ctx=malloc(sizeof(*ctx));
        if(!ctx){
            char *e=jrpc_error(req->id_str,JRPC_INTERNAL,"OOM");
            jrpc_req_free(req); return send_json(conn,500,e);
        }
        ctx->ci=ci; ctx->req=req; ctx->entry=entry;
        ci->state=CI_PROCESSING;
        MHD_suspend_connection(conn);
        if(!worker_pool_submit(pool,do_sync,(void *)ctx)){
            jrpc_req_free(req); free(ctx);
            ci->resp=jrpc_error("null",JRPC_QUEUE_FULL,"Server busy");
            ci->http_status=503;
            __atomic_store_n((int*)&ci->state,CI_READY,__ATOMIC_SEQ_CST);
            MHD_resume_connection(conn);
        }
        return MHD_YES;
    }

    /* Worker finished — send response */
    if(ci->state==CI_READY){
        if(!ci->resp)ci->resp=strdup("{\"error\":\"internal\"}");
        return send_json(conn,ci->http_status,ci->resp);
        /* ci->resp ownership transferred to MHD (MUST_FREE) */
    }

    LOGE("handle: unexpected state %d",(int)ci->state);
    return MHD_NO;
}

static void on_done(void *cls, struct MHD_Connection *c,
                    void **con_cls, enum MHD_RequestTerminationCode toe){
    (void)cls;(void)c;(void)toe;
    conn_info_t *ci=*con_cls;
    if(!ci)return;
    free(ci->req);
    /* ci->resp was transferred to MHD as MUST_FREE — already freed */
    free(ci); *con_cls=NULL;
}

struct MHD_Daemon *server_start(uint16_t port, unsigned int mhd_threads,
                                 worker_pool_t *pool){
    unsigned int flags=MHD_USE_INTERNAL_POLLING_THREAD|
                       MHD_ALLOW_SUSPEND_RESUME|
                       MHD_USE_ERROR_LOG;
    struct MHD_Daemon *d=MHD_start_daemon(
        flags,port,NULL,NULL,handle,pool,
        MHD_OPTION_THREAD_POOL_SIZE, mhd_threads,
        MHD_OPTION_CONNECTION_LIMIT, (unsigned int)MAX_CONNS,
        MHD_OPTION_CONNECTION_TIMEOUT,(unsigned int)CONN_TIMEOUT_SECS,
        MHD_OPTION_NOTIFY_COMPLETED, on_done,NULL,
        MHD_OPTION_END);
    if(!d){LOGE("MHD_start_daemon failed port=%d",(int)port);return NULL;}
    LOGI("server port=%d mhd_threads=%u max_conn=%d",(int)port,mhd_threads,MAX_CONNS);
    return d;
}
void server_stop(struct MHD_Daemon *d){if(d){MHD_stop_daemon(d);LOGI("server stopped");}}