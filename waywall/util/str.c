#include "util/str.h"
#include <stddef.h>

struct hdr {
    size_t len, cap;
    char data[];
};

#define HDRSZ sizeof(struct hdr)

#define strhdr(str) ((struct hdr *)((char *)str - HDRSZ))
#define hdrstr(hdr) ((hdr)->data)

str
str_append(str dst, const char *src) {
    struct hdr *hdst = strhdr(dst);
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
    return hdrstr(hdst);
}

void
str_clear(str str) {
    struct hdr *hdr = strhdr(str);
    hdr->len = 0;
}

void
str_free(str str) {
    struct hdr *hdr = strhdr(str);
    free(hdr);
}

str
str_new() {
    struct hdr *hdr = zalloc(1, sizeof(*hdr));
    return hdrstr(hdr);
}
