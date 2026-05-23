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
#include <pthread.h>
#include <getopt.h>

/* ── Globals ──────────────────────────────────────────────────────────── */
volatile log_level_t g_log_level = LOG_LEVEL_INFO;
pthread_mutex_t      g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static volatile sig_atomic_t g_running = 1;
static struct MHD_Daemon    *g_daemon  = NULL;
static worker_pool_t        *g_pool    = NULL;

/* ── Signal handling ──────────────────────────────────────────────────── */
static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static void setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    /* Ignore SIGPIPE — broken client connections must not kill us */
    signal(SIGPIPE, SIG_IGN);
}

/* ── CPU count ────────────────────────────────────────────────────────── */
static int cpu_count(void) {
#ifdef __APPLE__
    long n = 4;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    return (n > 0) ? (int)n : 1;
}

/* ── Usage ────────────────────────────────────────────────────────────── */
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -p <port>      Listen port          (default: %d)\n"
        "  -t <threads>   Worker thread count  (default: max(1, nCPU/2))\n"
        "  -q <depth>     Worker queue depth   (default: 256)\n"
        "  -l <level>     Log level 0=debug..3=error (default: 1=info)\n"
        "  -h             Show this help\n",
        prog, DEFAULT_PORT);
}

/* ── Main ─────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    uint16_t port         = DEFAULT_PORT;
    int      worker_count = 0;   /* 0 = auto (nCPU/2) */
    int      queue_depth  = 256;
    int      log_level    = LOG_LEVEL_INFO;

    int opt;
    while ((opt = getopt(argc, argv, "p:t:q:l:h")) != -1) {
        switch (opt) {
        case 'p': port         = (uint16_t)atoi(optarg);  break;
        case 't': worker_count = atoi(optarg);             break;
        case 'q': queue_depth  = atoi(optarg);             break;
        case 'l': log_level    = atoi(optarg);             break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    g_log_level = (log_level_t)log_level;

    if (worker_count <= 0) {
        int cpus  = cpu_count();
        worker_count = cpus / 2;
        if (worker_count < 1) worker_count = 1;
    }

    /* MHD thread pool: same size as worker pool for balanced I/O */
    unsigned int mhd_threads = (unsigned int)worker_count;

    LOGI("═══════════════════════════════════════════");
    LOGI("  JSONRPC 2.0 Server (MHD + yyjson)");
    LOGI("  port=%d  mhd_threads=%u  worker_threads=%d",
         (int)port, mhd_threads, worker_count);
    LOGI("  max_concurrent_conns=%d  queue_depth=%d",
         MAX_CONCURRENT_CONNS, queue_depth);
    LOGI("═══════════════════════════════════════════");

    setup_signals();

    /* Initialise subsystems */
    task_manager_init();
    methods_init();

    g_pool = worker_pool_create(worker_count, queue_depth);
    if (!g_pool) {
        LOGE("Failed to create worker pool");
        return 1;
    }
    g_worker_pool = g_pool;  /* expose to methods.c */

    g_daemon = server_start(port, mhd_threads, g_pool);
    if (!g_daemon) {
        LOGE("Failed to start MHD daemon");
        worker_pool_destroy(g_pool);
        task_manager_destroy();
        return 1;
    }

    LOGI("Server ready. Press Ctrl-C to stop.");

    /* Main loop: sleep until signal */
    while (g_running)
        sleep(1);

    LOGI("Shutting down...");
    server_stop(g_daemon);
    worker_pool_destroy(g_pool);
    task_manager_destroy();
    pthread_mutex_destroy(&g_log_mutex);

    LOGI("Goodbye.");
    return 0;
}
