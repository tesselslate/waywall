#include "util/str.h"
#include "util/alloc.h"
#include "util/list.h"
#include <stdbit.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

LIST_DEFINE(str, list_str);
LIST_DEFINE_IMPL(str, list_str);

static constexpr ssize_t STRBUF_BASE_CAP = 64;

static inline void
grow(strbuf *buf, ssize_t cap) {
    // include space for null terminator
    if (cap + 1 <= buf->cap) {
        return;
    }

    buf->cap = stdc_bit_ceil((size_t)cap + 1);
    buf->data = realloc(buf->data, buf->cap);
    check_alloc(buf->data);

    memset(buf->data + buf->len, 0, buf->cap - buf->len);
}

static inline struct strs
strs_from_list(struct list_str list) {
    return (struct strs){
        .len = list.len,
        .data = list.data,
    };
}

void
strbuf_append_char(strbuf *dst, char src) {
    grow(dst, dst->len + 1);
    dst->data[dst->len] = src;
    dst->len++;
}

void
strbuf_append_cstr(strbuf *dst, const char *src) {
    ssize_t len = strlen(src);
    grow(dst, dst->len + len);

    memcpy(dst->data + dst->len, src, len);
    dst->len += len;
}

void
strbuf_append_buf(strbuf *dst, strbuf src) {
    grow(dst, dst->len + src.len);

    memcpy(dst->data + dst->len, src.data, src.len);
    dst->len += src.len;
}

void
strbuf_append_str(strbuf *dst, str src) {
    grow(dst, dst->len + src.len);

    memcpy(dst->data + dst->len, src.data, src.len);
    dst->len += src.len;
}

void
strbuf_clear(strbuf *buf) {
    buf->len = 0;
}

strbuf
strbuf_clone(strbuf buf) {
    char *data = zalloc(buf.cap, 1);
    memcpy(data, buf.data, buf.len);

    return (strbuf){
        .cap = buf.cap,
        .len = buf.len,
        .data = data,
    };
}

char *
strbuf_clone_cstr(strbuf buf) {
    char *cstr = zalloc(buf.len + 1, 1);
    if (buf.len) {
        memcpy(cstr, buf.data, buf.len);
    }
    return cstr;
}

void
strbuf_free(strbuf *buf) {
    free(buf->data);

    *buf = (strbuf){};
}

strbuf
strbuf_new() {
    strbuf buf = {
        .cap = STRBUF_BASE_CAP,
        .data = zalloc(STRBUF_BASE_CAP, 1),
    };
    return buf;
}

str
strbuf_view(strbuf buf) {
    return (str){buf.len, buf.data};
}

strbuf
str_clone(str s) {
    // include space for null terminator
    ssize_t cap = stdc_bit_ceil((size_t)(s.len + 1));
    char *data = zalloc(cap, 1);
    memcpy(data, s.data, s.len);

    return (strbuf){
        .len = s.len,
        .cap = cap,
        .data = data,
    };
}

char *
str_clone_cstr(str s) {
    char *buf = zalloc(s.len + 1, 1);
    memcpy(buf, s.data, s.len);
    return buf;
}

bool
str_eq(str a, str b) {
    if (a.len != b.len) {
        return false;
    }
    if (a.len == b.len && a.len == 0) {
        return true;
    }

    return (memcmp(a.data, b.data, a.len) == 0);
}

str
str_from(const char *cstr) {
    ssize_t len = strlen(cstr);
    return (str){len, cstr};
}

ssize_t
str_index(str s, char needle, ssize_t start) {
    char *ptr = memchr(s.data + start, needle, s.len - start);

    return (ptr ? ptr - s.data : -1);
}

str
str_slice(str s, ssize_t start, ssize_t end) {
    ww_assert(start >= 0);
    ww_assert(start <= end);
    ww_assert(end <= s.len);

    return (str){end - start, s.data + start};
}

struct strs
str_split(str s, char sep) {
    struct list_str list = list_str_create();

    for (ssize_t pos = 0; pos < s.len;) {
        ssize_t next = str_index(s, sep, pos);
        if (next == -1) {
            next = s.len;
        }

        list_str_append(&list, str_slice(s, pos, next));

        pos = next + 1;

        if (pos == s.len) {
            list_str_append(&list, str_slice(s, s.len, s.len));
        }
    }

    return strs_from_list(list);
}

void
strs_free(struct strs strs) {
    free(strs.data);
}

ssize_t
strs_index(struct strs strs, str s, ssize_t start) {
    for (ssize_t i = start; i < strs.len; i++) {
        if (str_eq(strs.data[i], s)) {
            return i;
        }
    }

    return -1;
}
