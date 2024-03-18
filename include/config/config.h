#ifndef WAYWALL_CONFIG_CONFIG_H
#define WAYWALL_CONFIG_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

struct wall;

struct config {
    struct {
        char *counter_path;
    } general;
    struct {
        struct {
            char *layout;
            char *model;
            char *rules;
            char *variant;
            char *options;
        } keymap;

        int repeat_rate, repeat_delay;
        double sens;
        bool confine;
    } input;
    struct {
        bool handle_death;
        bool handle_manual;
        bool handle_preview_percent;
        bool handle_preview_start;
        bool handle_resize;
        bool handle_spawn;
    } layout;
    struct {
        uint8_t background[4];
        char *cursor_theme;
        char *cursor_icon;
        int cursor_size;
    } theme;

    struct lua_State *L;
};

struct config *config_create();
void config_destroy(struct config *cfg);
int config_load(struct config *cfg);

#endif
