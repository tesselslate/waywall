#ifndef WAYWALL_UTIL_STR_H
#define WAYWALL_UTIL_STR_H

#include "util/alloc.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef char *str;

str str_append(str dst, const char *src);
void str_clear(str str);
void str_free(str str);
str str_new();

#endif
