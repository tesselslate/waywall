#ifndef WAYWALL_UTIL_ALLOC_H
#define WAYWALL_UTIL_ALLOC_H

#include "util/prelude.h"
#include <stddef.h>
#include <stdlib.h>

#define check_alloc(data)                                                                          \
    do {                                                                                           \
        if (__builtin_expect(!(data), 0)) {                                                        \
            ww_panic("allocation failed");                                                         \
        }                                                                                          \
    } while (0)

static inline void *
zalloc(size_t nmemb, size_t size) {
    void *data = calloc(nmemb, size);
    if (__builtin_expect(!data, 0)) {
        ww_panic("allocation failed");
    }
    return data;
}

#endif
