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

#define ww_log(lvl, fmt, ...) _ww_log(lvl, "[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
#define ww_log_errno(lvl, fmt, ...)                                                                \
    _ww_log(lvl, "[%s:%d] " fmt ": %s", __FILE__, __LINE__, ##__VA_ARGS__, strerror(errno))

#define ww_assert(x)                                                                               \
    ({                                                                                             \
        if (!(x)) {                                                                                \
            _ww_panic(__FILE__, __LINE__, "assert failed: '" #x "'");                              \
        }                                                                                          \
    })
#define ww_panic(msg) _ww_panic(__FILE__, __LINE__, "panic: " msg)
#define ww_unreachable()                                                                           \
    ({                                                                                             \
        ww_panic("unreachable");                                                                   \
        __builtin_unreachable();                                                                   \
    })

enum ww_log_level {
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
};

void _ww_log(enum ww_log_level level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
noreturn void _ww_panic(const char *file, int line, const char *msg);

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

#endif
