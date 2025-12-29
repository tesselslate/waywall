#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define WW_DEBUG(key, val)                                                                         \
    do {                                                                                           \
        if (util_debug_enabled) {                                                                  \
            util_debug_data.key = (val);                                                           \
        }                                                                                          \
    } while (0)

extern bool util_debug_enabled;

extern struct util_debug {
    struct {
        ssize_t num_pressed;

        uint32_t remote_mods_serialized;
        uint32_t remote_mods_depressed;
        uint32_t remote_mods_latched;
        uint32_t remote_mods_locked;
        uint32_t remote_group;

        int32_t remote_repeat_rate;
        int32_t remote_repeat_delay;

        bool active;
    } keyboard;

    struct {
        double x, y;

        bool active;
    } pointer;

    struct {
        int32_t width, height;

        bool fullscreen;
    } ui;
} util_debug_data;

bool util_debug_init();
const char *util_debug_str();
