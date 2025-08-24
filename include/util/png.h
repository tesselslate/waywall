#ifndef WAYWALL_UTIL_PNG_H
#define WAYWALL_UTIL_PNG_H

#include <stddef.h>
#include <stdint.h>

struct util_png {
    char *data;
    size_t size;

    uint32_t width, height;
};

struct util_png util_png_decode(const char *path, int max_size);

#endif
