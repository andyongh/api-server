#pragma once
#include "worker_pool.h"
#include <microhttpd.h>
#include <stdint.h>

#define DEFAULT_PORT       8080
#define MAX_BODY_SIZE      (1024*1024)
#define MAX_CONNS          3
#define CONN_TIMEOUT_SECS  30

struct MHD_Daemon *server_start(uint16_t port, unsigned int mhd_threads,
                                 worker_pool_t *pool);
void server_stop(struct MHD_Daemon *d);