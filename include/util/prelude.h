#ifndef WAYWALL_UTIL_PRELUDE_H
#define WAYWALL_UTIL_PRELUDE_H

#include <stdnoreturn.h>

#define STATIC_ARRLEN(x) (sizeof((x)) / sizeof((x)[0]))
#define STATIC_STRLEN(x) (sizeof((x)) - 1)
#define static_assert(x) _Static_assert(x, #x)

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

noreturn void util_panic(const char *file, int line, const char *msg);

#endif
