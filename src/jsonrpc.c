#include "jsonrpc.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Serialise a yyjson id value to a short JSON literal string */
static void ser_id(yyjson_val *v, char *buf, size_t n){
    if(!v||yyjson_is_null(v))   {snprintf(buf,n,"null");return;}
    if(yyjson_is_sint(v))       {snprintf(buf,n,"%lld",(long long)yyjson_get_sint(v));return;}
    if(yyjson_is_uint(v))       {snprintf(buf,n,"%llu",(unsigned long long)yyjson_get_uint(v));return;}
    if(yyjson_is_str(v)){
        const char *s=yyjson_get_str(v);
        if(strlen(s)+4<n){buf[0]='"';strcpy(buf+1,s);strcat(buf,"\"");}
        else snprintf(buf,n,"null");
        return;
    }
    snprintf(buf,n,"null");
}

/*
 * Build params JSON without "session" key.
 * Uses yyjson_val_write() on each key and value separately —
 * avoids yyjson_mut_rawn() which stores a pointer without copying
 * and causes a use-after-free when the raw string is freed.
 */
static char *strip_session(yyjson_val *params){
    if(!params||!yyjson_is_obj(params))return strdup("{}");
    size_t cap=512,sz=0;
    char *buf=malloc(cap);
    if(!buf)return strdup("{}");
    buf[sz++]='{';
    bool first=true;
    yyjson_val *key;
    yyjson_obj_iter it=yyjson_obj_iter_with(params);
    while((key=yyjson_obj_iter_next(&it))){
        yyjson_val *val=yyjson_obj_iter_get_val(key);
        const char *k=yyjson_get_str(key);
        if(!k||strcmp(k,"session")==0)continue;
        size_t kl,vl;
        char *kr=yyjson_val_write(key,0,&kl); /* includes quotes */
        char *vr=yyjson_val_write(val,0,&vl);
        if(!kr||!vr){free(kr);free(vr);continue;}
        size_t need=(first?0:1)+kl+1+vl;
        if(sz+need+2>cap){cap=(sz+need+256)*2;char*nb=realloc(buf,cap);
            if(!nb){free(kr);free(vr);break;}buf=nb;}
        if(!first)buf[sz++]=',';
        memcpy(buf+sz,kr,kl);sz+=kl;
        buf[sz++]=':';
        memcpy(buf+sz,vr,vl);sz+=vl;
        free(kr);free(vr);first=false;
    }
    if(sz+2>cap){buf=realloc(buf,sz+2);}
    buf[sz++]='}'; buf[sz]='\0';
    return buf;
}

jrpc_req_t *jrpc_parse(const char *data, size_t len, char **err_out){
#define ERR(code,msg) do{if(err_out)*err_out=jrpc_error("null",code,msg);return NULL;}while(0)
    if(!data||len==0) ERR(JRPC_PARSE,"Empty body");
    yyjson_read_err re;
    yyjson_doc *doc=yyjson_read_opts((char*)data,len,YYJSON_READ_NOFLAG,NULL,&re);
    if(!doc){char m[128];snprintf(m,sizeof(m),"JSON parse: %s",re.msg);ERR(JRPC_PARSE,m);}
    yyjson_val *root=yyjson_doc_get_root(doc);
    if(!yyjson_is_obj(root)){yyjson_doc_free(doc);ERR(JRPC_INVALID_REQ,"Root must be object");}
    yyjson_val *ver=yyjson_obj_get(root,"jsonrpc");
    if(!ver||!yyjson_is_str(ver)||strcmp(yyjson_get_str(ver),"2.0")!=0){
        yyjson_doc_free(doc);ERR(JRPC_INVALID_REQ,"jsonrpc must be \"2.0\"");}
    yyjson_val *meth=yyjson_obj_get(root,"method");
    if(!meth||!yyjson_is_str(meth)){
        yyjson_doc_free(doc);ERR(JRPC_INVALID_REQ,"method missing or not string");}
    yyjson_val *id_v  =yyjson_obj_get(root,"id");
    yyjson_val *params=yyjson_obj_get(root,"params");
    const char *session=NULL;
    if(params&&yyjson_is_obj(params)){
        yyjson_val *sv=yyjson_obj_get(params,"session");
        if(sv&&yyjson_is_str(sv))session=yyjson_get_str(sv);
    }
    if(!session){yyjson_doc_free(doc);ERR(JRPC_AUTH,"Missing params.session");}

    jrpc_req_t *r=calloc(1,sizeof(*r));
    if(!r){yyjson_doc_free(doc);ERR(JRPC_INTERNAL,"OOM");}
    snprintf(r->method, sizeof(r->method), "%s",yyjson_get_str(meth));
    snprintf(r->session,sizeof(r->session),"%s",session);
    ser_id(id_v,r->id_str,sizeof(r->id_str));
    r->is_notification=(id_v==NULL);
    r->doc=doc; r->params=params;
    r->params_json=strip_session(params);
    LOGD("jrpc_parse method=%s id=%s",r->method,r->id_str);
    return r;
#undef ERR
}

void jrpc_req_free(jrpc_req_t *r){
    if(!r)return;
    yyjson_doc_free(r->doc);
    free(r->params_json);
    free(r);
}

char *jrpc_result(const char *id, const char *res){
    const char *i=id?id:"null", *v=res?res:"null";
    int n=snprintf(NULL,0,"{\"jsonrpc\":\"2.0\",\"result\":%s,\"id\":%s}",v,i);
    char *b=malloc(n+1);
    if(b)snprintf(b,n+1,"{\"jsonrpc\":\"2.0\",\"result\":%s,\"id\":%s}",v,i);
    return b;
}

char *jrpc_error(const char *id, int code, const char *msg){
    const char *i=id?id:"null";
    char safe[512]; size_t j=0;
    const char *m=msg?msg:"error";
    for(size_t k=0;m[k]&&j<sizeof(safe)-2;k++){
        if(m[k]=='"')safe[j++]='\\'; safe[j++]=m[k];}
    safe[j]='\0';
    int n=snprintf(NULL,0,
        "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":%d,\"message\":\"%s\"},\"id\":%s}",
        code,safe,i);
    char *b=malloc(n+1);
    if(b)snprintf(b,n+1,
        "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":%d,\"message\":\"%s\"},\"id\":%s}",
        code,safe,i);
    return b;
}

char *jrpc_accepted(const char *id, const char *task_id, int timeout_secs){
    char res[256];
    snprintf(res,sizeof(res),
        "{\"status\":\"accepted\",\"task_id\":\"%s\",\"timeout_secs\":%d}",
        task_id,timeout_secs);
    return jrpc_result(id,res);
}