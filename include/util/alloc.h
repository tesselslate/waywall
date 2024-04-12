#ifndef WAYWALL_UTIL_ALLOC_H
#define WAYWALL_UTIL_ALLOC_H

#include "util/prelude.h"
#include <stddef.h>
#include <stdlib.h>

static inline void
check_alloc(const void *data) {
    if (__builtin_expect(!data, 0)) {
        ww_panic("allocation failed");
    }
}

static inline void *
zalloc(size_t nmemb, size_t size) {
    void *data = calloc(nmemb, size);
    if (__builtin_expect(!data, 0)) {
        ww_panic("allocation failed");
    }
    return data;
}

#endif
