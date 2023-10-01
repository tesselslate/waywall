#ifndef __UTIL_H
#define __UTIL_H

#include <stdbool.h>

void __ww_assert(const char *file, const int line, const char *expr, bool value);

#ifndef WW_TRAP_ASSERT
#define ww_assert(expr) __ww_assert(__FILE__, __LINE__, #expr, expr)
#else
#define ww_assert(expr)                                                                            \
    if (expr)                                                                                      \
    __builtin_trap()
#endif

#define ARRAY_LEN(x) (sizeof((x)) / sizeof((x)[0]))
#define STRING_LEN(x) (ARRAY_LEN((x)) - 1)
#define STR(x) #x

#endif
