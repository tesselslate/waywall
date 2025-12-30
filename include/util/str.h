#pragma once

#include <sys/types.h>

typedef struct {
    ssize_t len, cap;
    char *data;
} strbuf;

#define strbuf_append(dst, src)                                                                    \
    _Generic((src),                                                                                \
        int: strbuf_append_char,                                                                   \
        strbuf: strbuf_append_buf,                                                                 \
        default: strbuf_append_cstr)(dst, src)

void strbuf_append_char(strbuf *dst, char src);
void strbuf_append_cstr(strbuf *dst, const char *src);
void strbuf_append_buf(strbuf *dst, strbuf src);
void strbuf_clear(strbuf *buf);
void strbuf_free(strbuf *buf);
strbuf strbuf_new();
