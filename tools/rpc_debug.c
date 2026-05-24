/*
 * rpc_debug — JSONRPC 2.0 debug client (no external deps)
 *
 * Usage: rpc_debug [opts] <method> [params_json]
 *   -h <host>   host      (default: 127.0.0.1)
 *   -p <port>   port      (default: 8080)
 *   -s <token>  session   (default: valid_testuser)
 *   -i <id>     req id    (default: 1)
 *   -P          poll until async task completes
 *   -t <secs>   poll timeout (default: 120)
 *   -v          verbose (show raw HTTP)
 *   -r          pretty-print JSON
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <time.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

static const char *g_host="127.0.0.1";
static int g_port=8080,g_id=1,g_timeout=120;
static const char *g_sess="valid_testuser";
static bool g_poll=false,g_verbose=false,g_pretty=false;

/* ── Dynamic buffer ────────────────────────────────────────────────────── */
typedef struct{char*d;size_t n,c;}buf_t;
static void bapp(buf_t*b,const char*s,size_t n){
    if(b->n+n+1>b->c){b->c=(b->n+n+256)*2;b->d=realloc(b->d,b->c);}
    memcpy(b->d+b->n,s,n);b->n+=n;b->d[b->n]='\0';}
static void bfree(buf_t*b){free(b->d);b->d=NULL;b->n=b->c=0;}

/* ── Pretty printer ────────────────────────────────────────────────────── */
static void pretty(const char *j){
    int d=0;bool ins=false;
    for(const char*p=j;*p;p++){
        if(ins){putchar(*p);
            if(*p=='\\'){ putchar(*++p); continue;}
            if(*p=='"')ins=false; continue;}
        switch(*p){
        case '"': ins=true;putchar('"');break;
        case '{':case'[': putchar(*p);putchar('\n');d++;
            for(int i=0;i<d*2;i++)putchar(' ');break;
        case '}':case']': putchar('\n');d--;
            for(int i=0;i<d*2;i++)putchar(' ');putchar(*p);break;
        case ',': putchar(',');putchar('\n');
            for(int i=0;i<d*2;i++)putchar(' ');break;
        case ':': putchar(':');putchar(' ');break;
        default:  putchar(*p);
        }
    }
    putchar('\n');
}

/* ── TCP connect ───────────────────────────────────────────────────────── */
static int tcp_conn(const char*h,int port){
    struct addrinfo hi={.ai_family=AF_UNSPEC,.ai_socktype=SOCK_STREAM},*res;
    char ps[16];snprintf(ps,sizeof(ps),"%d",port);
    if(getaddrinfo(h,ps,&hi,&res)!=0){fprintf(stderr,"resolve failed\n");return -1;}
    int fd=-1;
    for(struct addrinfo*r=res;r;r=r->ai_next){
        fd=socket(r->ai_family,r->ai_socktype,r->ai_protocol);
        if(fd<0)continue;
        if(connect(fd,r->ai_addr,r->ai_addrlen)==0)break;
        close(fd);fd=-1;
    }
    freeaddrinfo(res);
    if(fd<0)fprintf(stderr,"connect %s:%d failed\n",h,port);
    return fd;
}

/* ── HTTP POST ─────────────────────────────────────────────────────────── */
static int http_post(const char*body,size_t blen,buf_t*resp,int*status){
    int fd=tcp_conn(g_host,g_port);if(fd<0)return -1;
    char hdr[512];
    int hl=snprintf(hdr,sizeof(hdr),
        "POST / HTTP/1.1\r\nHost: %s:%d\r\n"
        "Content-Type: application/json\r\nContent-Length: %zu\r\n"
        "Connection: close\r\n\r\n",g_host,g_port,blen);
    if(g_verbose){fprintf(stderr,"\n=REQ=\n%s%s\n",hdr,body);}
    write(fd,hdr,hl);write(fd,body,blen);
    buf_t raw={0};char chunk[4096];ssize_t n;
    while((n=read(fd,chunk,sizeof(chunk)))>0)bapp(&raw,chunk,(size_t)n);
    close(fd);
    if(g_verbose){fprintf(stderr,"=RESP=\n%.*s\n",(int)raw.n,raw.d);}
    *status=0;
    if(raw.n>12){const char*sp=strchr(raw.d,' ');if(sp)*status=atoi(sp+1);}
    char*bs=strstr(raw.d,"\r\n\r\n");
    if(bs){bs+=4;bapp(resp,bs,raw.n-(size_t)(bs-raw.d));}
    bfree(&raw);return 0;
}

/* ── Build JSONRPC request ─────────────────────────────────────────────── */
static char *build_req(const char*method,const char*params_raw){
    /* merge session into params object */
    char merged[4096];
    const char *p=params_raw&&*params_raw?params_raw:"{}";
    size_t pl=strlen(p);
    if(pl>=2&&p[0]=='{'&&p[pl-1]=='}'){
        char inner[4096];
        snprintf(inner,sizeof(inner),"%.*s",(int)(pl-2),p+1);
        /* trim trailing whitespace */
        size_t il=strlen(inner);
        while(il>0&&(inner[il-1]==' '||inner[il-1]=='\n'||
                      inner[il-1]=='\r'||inner[il-1]=='\t'))inner[--il]='\0';
        if(il>0)
            snprintf(merged,sizeof(merged),"{\"session\":\"%s\",%s}",g_sess,inner);
        else
            snprintf(merged,sizeof(merged),"{\"session\":\"%s\"}",g_sess);
    } else {
        snprintf(merged,sizeof(merged),"{\"session\":\"%s\"}",g_sess);
    }
    int n=snprintf(NULL,0,
        "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s,\"id\":%d}",
        method,merged,g_id);
    char*b=malloc(n+1);
    snprintf(b,n+1,
        "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s,\"id\":%d}",
        method,merged,g_id);
    return b;
}

/* ── Minimal JSON string field extractor ───────────────────────────────── */
static bool jget(const char*json,const char*key,char*out,size_t osz){
    char k[128];snprintf(k,sizeof(k),"\"%s\"",key);
    const char*p=strstr(json,k);if(!p)return false;
    p+=strlen(k);while(*p==' '||*p==':')p++;
    if(*p!='"')return false;p++;
    size_t i=0;
    while(*p&&*p!='"'&&i<osz-1){if(*p=='\\'&&*(p+1))p++;out[i++]=*p++;}
    out[i]='\0';return true;
}

/* ── Poll loop ─────────────────────────────────────────────────────────── */
static int poll_task(const char*tid){
    time_t dl=time(NULL)+g_timeout;int iv=1;
    printf("Polling task %s (timeout=%ds)\n",tid,g_timeout);
    while(time(NULL)<dl){
        sleep(iv);if(iv<5)iv++;
        g_id++;
        char pp[128];snprintf(pp,sizeof(pp),"{\"task_id\":\"%s\"}",tid);
        char*req=build_req("task.status",pp);
        buf_t resp={0};int st=0;
        http_post(req,strlen(req),&resp,&st);free(req);
        char state[32]={0};jget(resp.d?resp.d:"","state",state,sizeof(state));
        printf("[%lds] state=%s\n",(long)(time(NULL)-(dl-g_timeout)),state[0]?state:"?");
        bool done=(strcmp(state,"completed")==0||strcmp(state,"failed")==0||
                   strcmp(state,"cancelled")==0||strcmp(state,"timed_out")==0);
        if(done){
            printf("\n── result ──\n");
            if(g_pretty&&resp.d)pretty(resp.d);
            else if(resp.d)puts(resp.d);
            bfree(&resp);return 0;
        }
        bfree(&resp);
    }
    fprintf(stderr,"poll timeout\n");return 1;
}

static void usage(const char*p){
    fprintf(stderr,
    "Usage: %s [opts] <method> [params_json]\n"
    "  -h host   (default: 127.0.0.1)\n"
    "  -p port   (default: 8080)\n"
    "  -s token  (default: valid_testuser)\n"
    "  -i id     request id (default: 1)\n"
    "  -P        poll until async task done\n"
    "  -t secs   poll timeout (default: 120)\n"
    "  -v        verbose\n"
    "  -r        pretty-print\n\n"
    "Methods: ping  echo  add  slow_compute\n"
    "         task.status  task.cancel  task.refresh  task.list\n"
    "Examples:\n"
    "  %s ping\n"
    "  %s -r add '{\"a\":3,\"b\":4}'\n"
    "  %s -r -P slow_compute '{\"n\":3}'\n"
    "  %s -s admin_root task.list\n",p,p,p,p,p);
}

int main(int argc,char*argv[]){
    int opt;
    while((opt=getopt(argc,argv,"h:p:s:i:Pt:vrH"))!=-1){
        switch(opt){
        case 'h':g_host=optarg;break;case 'p':g_port=atoi(optarg);break;
        case 's':g_sess=optarg;break;case 'i':g_id=atoi(optarg);break;
        case 'P':g_poll=true;break;case 't':g_timeout=atoi(optarg);break;
        case 'v':g_verbose=true;break;case 'r':g_pretty=true;break;
        case 'H':default:usage(argv[0]);return 0;
        }
    }
    if(optind>=argc){fprintf(stderr,"method required\n\n");usage(argv[0]);return 1;}
    const char *method=argv[optind];
    const char *params=(optind+1<argc)?argv[optind+1]:"{}";

    char *req=build_req(method,params);
    buf_t resp={0};int st=0;
    if(http_post(req,strlen(req),&resp,&st)!=0){free(req);return 1;}
    free(req);

    printf("HTTP %d\n",st);
    if(resp.d){
        if(g_pretty)pretty(resp.d); else puts(resp.d);}

    char status_f[32]={0},tid[64]={0};
    if(resp.d){jget(resp.d,"status",status_f,sizeof(status_f));
               jget(resp.d,"task_id",tid,sizeof(tid));}
    bfree(&resp);

    if(g_poll&&strcmp(status_f,"accepted")==0&&tid[0])
        return poll_task(tid);
    return (st>=200&&st<300)?0:1;
}