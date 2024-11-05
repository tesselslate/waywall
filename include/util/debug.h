#ifndef WAYWALL_UTIL_DEBUG_H
#define WAYWALL_UTIL_DEBUG_H

#include <stdbool.h>

#define WW_DEBUG(key, val)                                                                         \
    do {                                                                                           \
        if (util_debug_enabled) {                                                                  \
            util_debug_data.key = (val);                                                           \
        }                                                                                          \
    } while (0)

extern bool util_debug_enabled;

extern struct util_debug {

} util_debug_data;

bool util_debug_init();
const char *util_debug_str();

#endif
