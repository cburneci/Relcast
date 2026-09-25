/*
 * log.c - Minimal thread-safe logging facility
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE

#include "log.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

static int g_log_level = RC_LOG_INFO;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

void rc_log_set_level(int level)
{
    g_log_level = level;
}

static const char *level_str(rc_log_level_t level)
{
    switch (level) {
        case RC_LOG_ERROR: return "ERROR";
        case RC_LOG_WARN:  return "WARN ";
        case RC_LOG_INFO:  return "INFO ";
        case RC_LOG_DEBUG: return "DEBUG";
        default: return "?????";
    }
}

void rc_log(rc_log_level_t level, const char *tag, const char *fmt, ...)
{
    if ((int)level > g_log_level)
        return;

    char timebuf[32];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tmv);

    pthread_mutex_lock(&g_log_mutex);

    //FILE *out = (level == RC_LOG_ERROR) ? stderr : stdout;
    
    FILE *out = stdout; 
    fprintf(out, "[%s] [%s] [%s] ", timebuf, level_str(level), tag ? tag : "-");

    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);

    fputc('\n', out);
    fflush(out);

    pthread_mutex_unlock(&g_log_mutex);
}
