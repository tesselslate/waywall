#ifndef WAYWALL_CONFIG_CONFIG_H
#define WAYWALL_CONFIG_CONFIG_H

#include <luajit-2.1/lua.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct config {
    struct {
        bool jit;
        bool tearing;
    } experimental;

    struct {
        struct {
            char *layout;
            char *model;
            char *rules;
            char *variant;
            char *options;
        } keymap;

        struct {
            struct config_remap *data;
            size_t count;
        } remaps;

        int repeat_rate, repeat_delay;
        double sens;
        bool confine;
    } input;
    struct {
        uint8_t background[4];
        char *cursor_theme;
        char *cursor_icon;
        int cursor_size;

        enum floating_anchor {
            ANCHOR_TOPLEFT,
            ANCHOR_TOP,
            ANCHOR_TOPRIGHT,
            ANCHOR_LEFT,
            ANCHOR_RIGHT,
            ANCHOR_BOTTOMLEFT,
            ANCHOR_BOTTOMRIGHT,
            ANCHOR_NONE,
        } ninb_anchor;
    } theme;

    struct config_vm *vm;
};

struct config_action {
    enum config_action_type {
        CONFIG_ACTION_NONE,
        CONFIG_ACTION_BUTTON,
        CONFIG_ACTION_KEY,
    } type;

    uint32_t data;
    uint32_t modifiers;
};

enum config_remap_type {
    CONFIG_REMAP_NONE,
    CONFIG_REMAP_KEY,
    CONFIG_REMAP_BUTTON,
};

struct config_remap {
    enum config_remap_type src_type, dst_type;
    uint32_t src_data, dst_data;
};

struct config *config_create();
void config_destroy(struct config *cfg);
int config_load(struct config *cfg, const char *profile);

#endif
