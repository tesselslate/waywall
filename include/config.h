#ifndef WAYWALL_CONFIG_H
#define WAYWALL_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

struct config_vm {
    struct lua_State *L;
};

struct config {
    struct {
        struct xkb_context *xkb_ctx;
        struct xkb_keymap *xkb_keymap;
        bool custom_keymap;

        int repeat_rate, repeat_delay;
        double sens;
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

    struct config_vm vm;
};

struct config *config_create();
void config_destroy(struct config *cfg);
int config_populate(struct config *cfg);

#endif
