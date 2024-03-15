#ifndef WAYWALL_CONFIG_H
#define WAYWALL_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

struct wall;

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

    struct config_vm {
        struct lua_State *L;
    } vm;
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

struct config *config_create();
void config_destroy(struct config *cfg);
int config_do_action(struct config *cfg, struct wall *wall, struct config_action action);
int config_populate(struct config *cfg);

#endif
