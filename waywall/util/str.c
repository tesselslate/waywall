#include "util/str.h"
#include "util/alloc.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

struct hdr {
    size_t len, cap;
    char data[];
};

static constexpr size_t HDRSZ = sizeof(struct hdr);

static inline struct hdr *
strbuf_hdr(strbuf buf) {
    return (struct hdr *)((char *)buf - HDRSZ);
}

static inline strbuf
hdr_strbuf(struct hdr *hdr) {
    return hdr->data;
}

void
strbuf_append(strbuf *dst, const char *src) {
    struct hdr *hdst = strbuf_hdr(*dst);
    size_t srclen = strlen(src);

    size_t need_cap = srclen + hdst->len + 1;
    if (hdst->cap < need_cap) {
        hdst = realloc(hdst, HDRSZ + need_cap);
        check_alloc(hdst);
        hdst->cap = need_cap;
    }

    memcpy(hdst->data + hdst->len, src, srclen);
    hdst->len += srclen;
    hdst->data[hdst->len] = '\0';
    *dst = hdr_strbuf(hdst);
}

void
strbuf_clear(strbuf buf) {
    struct hdr *hdr = strbuf_hdr(buf);
    hdr->len = 0;
}

void
strbuf_free(strbuf buf) {
    struct hdr *hdr = strbuf_hdr(buf);
    free(hdr);
}

strbuf
strbuf_new() {
    struct hdr *hdr = zalloc(1, sizeof(*hdr) + 1);
    return hdr_strbuf(hdr);
}
