#ifndef WAYWALL_UTIL_STR_H
#define WAYWALL_UTIL_STR_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

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
