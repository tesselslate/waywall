#ifndef WAYWALL_UTIL_LIST_H
#define WAYWALL_UTIL_LIST_H

#include "util/alloc.h"
#include "util/prelude.h"
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#define LIST_DEFINE(type, name)                                                                    \
    struct name {                                                                                  \
        ssize_t len, cap;                                                                          \
        type *data;                                                                                \
    };

#define LIST_DEFINE_IMPL(type, name)                                                               \
    WW_MAYBE_UNUSED static inline void name##_append(struct name *list, type item) {               \
        if (list->len == list->cap) {                                                              \
            ssize_t cap = list->cap * 2;                                                           \
            list->data = realloc(list->data, sizeof(*list->data) * cap);                           \
            check_alloc(list->data);                                                               \
        }                                                                                          \
                                                                                                   \
        list->data[list->len++] = item;                                                            \
    }                                                                                              \
                                                                                                   \
    WW_MAYBE_UNUSED static inline void name##_remove(struct name *list, ssize_t index) {           \
        ww_assert(list->len > index);                                                              \
                                                                                                   \
        memmove(list->data + index, list->data + index + 1,                                        \
                (list->len - index - 1) * sizeof(*list->data));                                    \
        list->len--;                                                                               \
    }                                                                                              \
                                                                                                   \
    WW_MAYBE_UNUSED static inline struct name name##_create() {                                    \
        struct name list = {0};                                                                    \
        list.cap = 8;                                                                              \
        list.data = zalloc(8, sizeof(*list.data));                                                 \
                                                                                                   \
        return list;                                                                               \
    }                                                                                              \
                                                                                                   \
    WW_MAYBE_UNUSED static inline void name##_destroy(struct name *list) {                         \
        free(list->data);                                                                          \
        list->len = 0;                                                                             \
        list->cap = 0;                                                                             \
        list->data = NULL;                                                                         \
    }

LIST_DEFINE(uint32_t, list_uint32);
LIST_DEFINE(int, list_int);

LIST_DEFINE_IMPL(uint32_t, list_uint32);
LIST_DEFINE_IMPL(int, list_int);

#endif
