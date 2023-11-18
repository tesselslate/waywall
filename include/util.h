#ifndef WAYWALL_UTIL_H
#define WAYWALL_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>

void _ww_assert(const char *file, const int line, const char *expr, bool value);

#define static_assert(x, y) _Static_assert(x, y)

#ifndef WW_TRAP_ASSERT
#define ww_assert(expr) _ww_assert(__FILE__, __LINE__, #expr, expr)
#else
#define ww_assert(expr)                                                                            \
    if (expr)                                                                                      \
    __builtin_trap()
#endif

#define ww_unreachable()                                                                           \
    ww_assert(!"unreachable");                                                                     \
    __builtin_unreachable()

#define ARRAY_LEN(x) (sizeof((x)) / sizeof((x)[0]))
#define STRING_LEN(x) (ARRAY_LEN((x)) - 1)
#define STR(x) #x

#define MAX_INSTANCES 128
#define INSTANCE_BITFIELD_WIDTH (MAX_INSTANCES / 8)

static inline bool
ww_util_parse_color(float value[4], const char *in) {
    size_t len = strlen(in);
    bool maybe_valid_rgb = len == 6 || (len == 7 && in[0] == '#');
    bool maybe_valid_rgba = len == 8 || (len == 9 && in[0] == '#');
    if (!maybe_valid_rgb && !maybe_valid_rgba) {
        return false;
    }
    int r, g, b, a;
    if (maybe_valid_rgb) {
        int n = sscanf(in[0] == '#' ? in + 1 : in, "%02x%02x%02x", &r, &g, &b);
        if (n != 3) {
            return false;
        }
        value[0] = r / 255.0;
        value[1] = g / 255.0;
        value[2] = b / 255.0;
        value[3] = 1.0;
    } else {
        int n = sscanf(in[0] == '#' ? in + 1 : in, "%02x%02x%02x%02x", &r, &g, &b, &a);
        if (n != 4) {
            return false;
        }
        value[0] = r / 255.0;
        value[1] = g / 255.0;
        value[2] = b / 255.0;
        value[3] = a / 255.0;
    }
    return true;
}

#endif
