#pragma once

typedef char *str;

void str_append(str *dst, const char *src);
void str_clear(str str);
void str_free(str str);
str str_new();
