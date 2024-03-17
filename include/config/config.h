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
        uint8_t background[4];
        char *cursor_theme;
        char *cursor_icon;
        int cursor_size;
    } theme;
    struct {
        int width, height;
        int stretch_width, stretch_height;
    } wall;

    struct lua_State *L;
};

struct config *config_create();
void config_destroy(struct config *cfg);
int config_load(struct config *cfg);

#endif
