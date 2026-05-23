#pragma once
#include <stdio.h>
#include <time.h>
#include <pthread.h>

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
} log_level_t;

extern volatile log_level_t g_log_level;
extern pthread_mutex_t      g_log_mutex;

static inline const char *_ll_str(log_level_t l) {
    switch (l) {
    case LOG_LEVEL_DEBUG: return "DBG";
    case LOG_LEVEL_INFO:  return "INF";
    case LOG_LEVEL_WARN:  return "WRN";
    case LOG_LEVEL_ERROR: return "ERR";
    default:              return "???";
    }
}

/* Use GNU ##__VA_ARGS__ extension to handle zero variadic args cleanly. */
#define LOG(level, fmt, ...) do { \
    if ((level) >= g_log_level) { \
        time_t _t = time(NULL); \
        struct tm _tm; \
        localtime_r(&_t, &_tm); \
        char _ts[20]; \
        strftime(_ts, sizeof(_ts), "%H:%M:%S", &_tm); \
        pthread_mutex_lock(&g_log_mutex); \
        fprintf(stderr, "[%s][%s][T%04lu] " fmt "\n", _ts, \
                _ll_str(level), (unsigned long)pthread_self() % 9999, \
                ##__VA_ARGS__); \
        pthread_mutex_unlock(&g_log_mutex); \
    } \
} while (0)

#define LOGD(fmt, ...) LOG(LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) LOG(LOG_LEVEL_INFO,  fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) LOG(LOG_LEVEL_WARN,  fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) LOG(LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)