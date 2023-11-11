#ifndef WAYWALL_UTIL_H
#define WAYWALL_UTIL_H

#include <stdbool.h>

void _ww_assert(const char *file, const int line, const char *expr, bool value);

#define static_assert(x, y) _Static_assert(x, y)

#ifndef WW_TRAP_ASSERT
#define ww_assert(expr) _ww_assert(__FILE__, __LINE__, #expr, expr)
#else
#define ww_assert(expr)                                                                            \
    if (expr)                                                                                      \
    __builtin_trap()
#endif

#define ww_unreachable()                                                                           \
    ww_assert(!"unreachable");                                                                     \
    __builtin_unreachable()

#define ARRAY_LEN(x) (sizeof((x)) / sizeof((x)[0]))
#define STRING_LEN(x) (ARRAY_LEN((x)) - 1)
#define STR(x) #x

#endif
