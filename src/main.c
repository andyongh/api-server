#include "server.h"
#include "worker_pool.h"
#include "task_manager.h"
#include "methods.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>

volatile log_level_t g_log_level = LOG_LEVEL_DEBUG;
pthread_mutex_t      g_log_mutex  = PTHREAD_MUTEX_INITIALIZER;

static volatile sig_atomic_t g_run = 1;
static struct MHD_Daemon    *g_daemon = NULL;
static worker_pool_t        *g_pool   = NULL;

static void sig_handler(int s){(void)s;g_run=0;}

static void setup_signals(void){
    struct sigaction sa; memset(&sa,0,sizeof(sa)); sa.sa_handler=sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,&sa,NULL); sigaction(SIGTERM,&sa,NULL);
    signal(SIGPIPE,SIG_IGN);
}

// static int cpu_count(void){long n=sysconf(_SC_NPROCESSORS_ONLN);return n>0?(int)n:1;}
static int cpu_count(void){
    return 4;
}

static void usage(const char *p){
    fprintf(stderr,"Usage: %s [-p port] [-t workers] [-q qcap] [-l 0-3]\n",p);}

int main(int argc, char *argv[]){
    uint16_t port=DEFAULT_PORT;
    int workers=0, qcap=256, lv=LOG_LEVEL_DEBUG;
    int opt;
    while((opt=getopt(argc,argv,"p:t:q:l:h"))!=-1){
        switch(opt){
        case 'p':port=(uint16_t)atoi(optarg);break;
        case 't':workers=atoi(optarg);break;
        case 'q':qcap=atoi(optarg);break;
        case 'l':lv=atoi(optarg);break;
        case 'h':usage(argv[0]);return 0;
        default: usage(argv[0]);return 1;
        }
    }
    g_log_level=(log_level_t)lv;
    if(workers<=0){int c=cpu_count();workers=c/2<1?1:c/2;}

    LOGI("JSONRPC server  port=%d  mhd_threads=%d  worker_threads=%d  max_conn=%d",
         (int)port,workers,workers,MAX_CONNS);

    setup_signals();
    task_manager_init();
    methods_init();

    g_pool=worker_pool_create(workers,qcap);
    if(!g_pool){LOGE("worker pool failed");return 1;}
    g_worker_pool=g_pool;

    g_daemon=server_start(port,(unsigned int)workers,g_pool);
    if(!g_daemon){worker_pool_destroy(g_pool);task_manager_destroy();return 1;}

    LOGI("ready. Ctrl-C to stop.");
    while(g_run)sleep(1);

    LOGI("shutting down...");
    server_stop(g_daemon);
    worker_pool_destroy(g_pool);
    task_manager_destroy();
    pthread_mutex_destroy(&g_log_mutex);
    LOGI("bye.");
    return 0;
}