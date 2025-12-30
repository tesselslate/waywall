#include "util/str.h"
#include "util/alloc.h"
#include <stdbit.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static constexpr ssize_t STRBUF_BASE_CAP = 64;

static inline void
grow(strbuf *buf, ssize_t cap) {
    ww_assert(cap > 0);

    // include space for null terminator
    if (cap + 1 <= buf->cap) {
        return;
    }

    buf->cap = stdc_bit_ceil((size_t)cap);
    buf->data = realloc(buf->data, buf->cap);
    check_alloc(buf->data);

    memset(buf->data + buf->len, 0, buf->cap - buf->len);
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
strbuf_clear(strbuf *buf) {
    buf->len = 0;
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
