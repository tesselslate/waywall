#ifndef WAYWALL_UTIL_LOG_H
#define WAYWALL_UTIL_LOG_H

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>

#define ww_log(lvl, fmt, ...) util_log(lvl, "[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
#define ww_log_errno(lvl, fmt, ...)                                                                \
    util_log(lvl, "[%s:%d] " fmt ": %s", __FILE__, __LINE__, ##__VA_ARGS__, strerror(errno))

enum ww_log_level {
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
};

void util_log(enum ww_log_level level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void util_log_va(enum ww_log_level, const char *fmt, va_list args, bool newline);

void util_log_init();
void util_log_set_file(int fd);

#endif
