#pragma once

#include "util/prelude.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define check_alloc(data)                                                                          \
    do {                                                                                           \
        if (__builtin_expect(!(data), 0)) {                                                        \
            ww_panic("allocation failed");                                                         \
        }                                                                                          \
    } while (0)

static inline char *
ww_strdup(const char *str) {
    ww_assert(str);

    char *dup = strdup(str);
    check_alloc(dup);
    return dup;
}

static inline void *
zalloc(size_t nmemb, size_t size) {
    void *data = calloc(nmemb, size);
    if (__builtin_expect(!data, 0)) {
        ww_panic("allocation failed");
    }
    return data;
}
