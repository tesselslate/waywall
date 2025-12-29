#pragma once

#include <stdbool.h>
#include <stdnoreturn.h>

#ifdef __GNUC__
#define WW_PRINTF(a, b) __attribute((format(printf, (a), (b))))
#define WW_MAYBE_UNUSED __attribute((unused))
#else
#define WW_PRINTF(a, b)
#define WW_MAYBE_UNUSED
#endif

#define STATIC_ARRLEN(x) (sizeof((x)) / sizeof((x)[0]))
#define STATIC_STRLEN(x) (sizeof((x)) - 1)
#define static_assert(x) _Static_assert(x, #x)

#define ww_assert(x)                                                                               \
    do {                                                                                           \
        if (__builtin_expect(!(bool)(x), 0)) {                                                     \
            util_panic("[%s:%d] assert failed: '" #x "'", __FILE__, __LINE__);                     \
        }                                                                                          \
    } while (0)
#define ww_panic(fmt, ...) util_panic("[%s:%d] panic: " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
#define ww_unreachable()                                                                           \
    do {                                                                                           \
        ww_panic("unreachable");                                                                   \
        __builtin_unreachable();                                                                   \
    } while (0)

noreturn void util_panic(const char *fmt, ...) WW_PRINTF(1, 2);
