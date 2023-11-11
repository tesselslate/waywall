#ifndef WAYWALL_STR_H
#define WAYWALL_STR_H

#include "util.h"
#include <stddef.h>
#include <string.h>

/*
 *  The str type provides a simple wrapper around null-terminated strings, storing length and
 *  capacity. It is mostly intended for simpler and safer appending, which is the most common
 *  string manipulation operation in waywall (due to handling paths).
 *
 *  All functions involving str objects accept and return values, not pointers.
 */
struct str {
    char *buf;
    size_t len, cap;
};

/*
 *  str_append appends string `b` to string `a`, returning the result.
 */
static inline struct str
str_append(struct str a, struct str b) {
    // The null terminator must fit.
    ww_assert(a.len + b.len < a.cap);

    memcpy(a.buf + a.len, b.buf, b.len);
    a.len += b.len;
    a.buf[a.len] = '\0';
    return a;
}

/*
 *  str_appendl appends the given string literal to `str`, returning the result.
 */
static inline struct str
str_appendl(struct str str, const char *literal) {
    size_t len = strlen(literal);

    // The null terminator must fit.
    ww_assert(str.len + len < str.cap);

    memcpy(str.buf + str.len, literal, len);
    str.len += len;
    str.buf[str.len] = '\0';
    return str;
}

/*
 *  str_copy copies the contents of string `dst` to string `src`, returning the result.
 */
static inline struct str
str_copy(struct str dst, struct str src) {
    // The null terminator must fit.
    ww_assert(dst.cap > src.len);

    memcpy(dst.buf, src.buf, src.len);
    dst.buf[src.len] = '\0';
    dst.len = src.len;
    return dst;
}

/*
 *  str_from creates a new immutable str from the given null-terminated string. It does not
 *  reallocate and reuses the buffer of the given string. Any attempts to mutate the returned
 *  str will trigger an assert.
 */
static inline struct str
str_from(char *cstr) {
    size_t len = strlen(cstr);
    return (struct str){cstr, len, 0};
}

/*
 *  str_into returns a null-terminated string for the given str.
 */
static inline const char *
str_into(struct str str) {
    return str.buf;
}

/*
 *  str_new creates a new empty string from the given buffer.
 */
static inline struct str
str_new(char *buf, size_t cap) {
    return (struct str){buf, 0, cap};
}

#endif
