#pragma once

typedef char *strbuf;

void strbuf_append(strbuf *dst, const char *src);
void strbuf_clear(strbuf buf);
void strbuf_free(strbuf buf);
strbuf strbuf_new();
