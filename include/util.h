#ifndef WAYWALL_UTIL_H
#define WAYWALL_UTIL_H

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum log_level {
    LOG_INFO,
    LOG_ERROR,
};

static inline void _ww_assert(const char *file, int line, const char *expr, bool value);
bool ww_util_parse_color(float value[4], const char *in);

#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
void
_ww_log(enum log_level level, const char *fmt, ...);

static inline void
_ww_assert(const char *file, int line, const char *expr, bool value) {
    if (!value) {
        fprintf(stderr, "[%s:%d] assert failed: '%s'\n", file, line, expr);
        exit(1);
    }
}

#define static_assert(x, y) _Static_assert(x, y)

#define ARRAY_LEN(x) (sizeof((x)) / sizeof((x)[0]))
#define STRING_LEN(x) (ARRAY_LEN((x)) - 1)
#define STR(x) #x

#define MAX_INSTANCES 128

#define ww_assert(expr) _ww_assert(__FILE__, __LINE__, #expr, expr)

// XXX: GNU (statement expression)
#define ww_unreachable()                                                                           \
    ({                                                                                             \
        ww_assert(!"unreachable");                                                                 \
        __builtin_unreachable();                                                                   \
        0;                                                                                         \
    })

#define LOG(lvl, fmt, ...) _ww_log((lvl), "[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
#define LOG_ERRNO(lvl, fmt, ...)                                                                   \
    _ww_log((lvl), "[%s:%d] " fmt ": %s", __FILE__, __LINE__, ##__VA_ARGS__, strerror(errno))

#endif
