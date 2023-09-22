#ifndef __UTIL_H
#define __UTIL_H

#include <stdbool.h>

void __ww_assert(const char *file, const int line, const char *expr, bool value);

#define ww_assert(expr) __ww_assert(__FILE__, __LINE__, #expr, expr)

#endif
