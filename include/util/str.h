#pragma once

#include <sys/types.h>

#define str_lit(lit) ((str){sizeof(lit) - 1, lit})

typedef struct {
    ssize_t len;
    const char *data;
} str;

typedef struct {
    ssize_t len, cap;
    char *data;
} strbuf;

struct strs {
    ssize_t len;
    str *data;
};

#define strbuf_append(dst, src)                                                                    \
    _Generic((src),                                                                                \
        int: strbuf_append_char,                                                                   \
        strbuf: strbuf_append_buf,                                                                 \
        str: strbuf_append_str,                                                                    \
        default: strbuf_append_cstr)(dst, src)

void strbuf_append_char(strbuf *dst, char src);
void strbuf_append_cstr(strbuf *dst, const char *src);
void strbuf_append_buf(strbuf *dst, strbuf src);
void strbuf_append_str(strbuf *dst, str src);
void strbuf_clear(strbuf *buf);
void strbuf_free(strbuf *buf);
strbuf strbuf_new();
str strbuf_view(strbuf buf);

strbuf str_clone(str s);
char *str_clone_cstr(str s);
bool str_eq(str a, str b);
str str_from(const char *cstr);
ssize_t str_index(str s, char needle, ssize_t start);
str str_slice(str s, ssize_t start, ssize_t end);
struct strs str_split(str s, char sep);

void strs_free(struct strs strs);
ssize_t strs_index(struct strs strs, str s, ssize_t start);
