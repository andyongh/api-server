#pragma once
#include "worker_pool.h"
#include <microhttpd.h>
#include <stdbool.h>

#define DEFAULT_PORT            8080
#define MAX_BODY_SIZE           (1024 * 1024)   /* 1 MB request cap     */
#define MAX_CONCURRENT_CONNS    3               /* MHD connection limit */
#define CONN_TIMEOUT_SECS       30

/**
 * Start the MHD daemon.
 * @param port          TCP port to listen on.
 * @param thread_count  MHD internal thread pool size.
 * @param pool          Worker pool for processing requests.
 * @return Pointer to daemon on success, NULL on failure.
 */
struct MHD_Daemon *server_start(uint16_t port,
                                 unsigned int thread_count,
                                 worker_pool_t *pool);

/** Stop and free the MHD daemon (blocks until all connections closed). */
void server_stop(struct MHD_Daemon *daemon);