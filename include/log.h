/*
 * log.h - Minimal thread-safe logging facility
 */
#ifndef RC_LOG_H
#define RC_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RC_LOG_ERROR = 0,
    RC_LOG_WARN  = 1,
    RC_LOG_INFO  = 2,
    RC_LOG_DEBUG = 3
} rc_log_level_t;

void rc_log_set_level(int level);
void rc_log(rc_log_level_t level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define rc_loge(tag, ...) rc_log(RC_LOG_ERROR, tag, __VA_ARGS__)
#define rc_logw(tag, ...) rc_log(RC_LOG_WARN,  tag, __VA_ARGS__)
#define rc_logi(tag, ...) rc_log(RC_LOG_INFO,  tag, __VA_ARGS__)
#define rc_logd(tag, ...) rc_log(RC_LOG_DEBUG, tag, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* RC_LOG_H */
