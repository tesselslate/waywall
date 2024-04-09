#ifndef WAYWALL_UTIL_H
#define WAYWALL_UTIL_H

/*
 * This is hopefully the only file in the codebase which will have some bad macro trickery. Sorry.
 * Some other files still make use of GNU extensions, though.
 */

#ifndef __GNUC__
#error "waywall requires GNU C extensions"
#endif

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <sys/types.h>

#define STATIC_ARRLEN(x) (sizeof((x)) / sizeof((x)[0]))
#define STATIC_STRLEN(x) (sizeof((x)) - 1)
#define static_assert(x) _Static_assert(x, #x)

#define ww_log(lvl, fmt, ...) util_log(lvl, "[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
#define ww_log_errno(lvl, fmt, ...)                                                                \
    util_log(lvl, "[%s:%d] " fmt ": %s", __FILE__, __LINE__, ##__VA_ARGS__, strerror(errno))

#define ww_assert(x)                                                                               \
    do {                                                                                           \
        if (__builtin_expect(!(bool)(x), 0)) {                                                     \
            util_panic(__FILE__, __LINE__, "assert failed: '" #x "'");                             \
        }                                                                                          \
    } while (0)
#define ww_panic(msg) util_panic(__FILE__, __LINE__, "panic: " msg)
#define ww_unreachable()                                                                           \
    do {                                                                                           \
        ww_panic("unreachable");                                                                   \
        __builtin_unreachable();                                                                   \
    } while (0)

enum ww_log_level {
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
};

void util_log(enum ww_log_level level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void util_log_init();
void util_log_va(enum ww_log_level, const char *fmt, va_list args, bool newline);
noreturn void util_panic(const char *file, int line, const char *msg);

struct str {
    char data[PATH_MAX];
    size_t len;
};

static inline bool
str_append(struct str *dst, const char *src) {
    size_t len = strlen(src);

    if (dst->len + len >= PATH_MAX - 1) {
        return false;
    }

    strcpy(dst->data + dst->len, src);
    dst->data[dst->len + len] = '\0';
    dst->len += len;
    return true;
}

static inline void
check_alloc(const void *data) {
    if (__builtin_expect(!data, 0)) {
        ww_panic("allocation failed");
    }
}

static inline void *
zalloc(size_t nmemb, size_t size) {
    void *data = calloc(nmemb, size);
    if (__builtin_expect(!data, 0)) {
        ww_panic("allocation failed");
    }
    return data;
}

#endif
