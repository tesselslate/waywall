#ifndef WAYWALL_SERVER_WL_SEAT_H
#define WAYWALL_SERVER_WL_SEAT_H

#include "config/config.h"
#include "util/list.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <xkbcommon/xkbcommon.h>

enum kb_modifier {
    KB_MOD_SHIFT = (1 << 0),
    KB_MOD_CAPS = (1 << 1),
    KB_MOD_CTRL = (1 << 2),
    KB_MOD_MOD1 = (1 << 3),
    KB_MOD_MOD2 = (1 << 4),
    KB_MOD_MOD3 = (1 << 5),
    KB_MOD_MOD4 = (1 << 6),
    KB_MOD_MOD5 = (1 << 7),
};

struct server_seat {
    struct wl_global *global;

    struct server *server;
    struct xkb_context *ctx;

    struct server_seat_config *config;

    struct {
        struct wl_keyboard *remote;

        struct server_seat_keymap {
            int32_t fd;
            uint32_t size;
            struct xkb_keymap *xkb;
            struct xkb_state *state;
        } remote_km;
        int32_t repeat_rate, repeat_delay;

        uint8_t mod_indices[8];
        struct list_uint32 pressed;
    } keyboard;
    struct {
        struct wl_pointer *remote;
        double x, y;
    } pointer;

    uint32_t last_serial;

    struct server_view *input_focus;
    struct wl_listener on_input_focus;

    const struct server_seat_listener *listener;
    void *listener_data;

    struct wl_list keyboards; // wl_resource link
    struct wl_list pointers;  // wl_resource link

    struct wl_listener on_keyboard;
    struct wl_listener on_pointer;

    struct wl_listener on_display_destroy;

    struct {
        struct wl_signal keyboard_enter; // data: uint32_t *
        struct wl_signal keyboard_leave; // data: uint32_t *
        struct wl_signal pointer_enter;  // data: uint32_t *
    } events;
};

struct server_seat_config {
    int repeat_rate, repeat_delay;
    struct server_seat_keymap keymap;

    struct server_seat_remaps {
        struct server_seat_remap {
            enum config_remap_type type;
            uint32_t src, dst;
        } *keys, *buttons;
        size_t num_keys, num_buttons;
    } remaps;
};

struct server_seat_listener {
    bool (*button)(void *data, uint32_t button, bool state);
    bool (*key)(void *data, size_t num_syms, const uint32_t syms[static num_syms], bool state);
    void (*modifiers)(void *data, uint32_t mods);
    void (*motion)(void *data, double x, double y);
};

struct syn_key {
    uint32_t keycode;
    bool press;
};

struct server_seat *server_seat_create(struct server *server, struct config *cfg);
void server_seat_send_click(struct server_seat *seat, struct server_view *view);
void server_seat_send_keys(struct server_seat *seat, struct server_view *view, size_t num_keys,
                           const struct syn_key[static num_keys]);
void send_keyboard_modifiers(struct server_seat *seat);
void server_seat_set_listener(struct server_seat *seat, const struct server_seat_listener *listener,
                              void *data);
void server_seat_use_config(struct server_seat *seat, struct server_seat_config *config);

struct server_seat_config *server_seat_config_create(struct server_seat *seat, struct config *cfg);
void server_seat_config_destroy(struct server_seat_config *config);

int server_seat_lua_set_keymap(struct server_seat *seat, const struct xkb_rule_names *rule_names);

#endif
