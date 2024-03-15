#ifndef WAYWALL_SERVER_WL_SEAT_H
#define WAYWALL_SERVER_WL_SEAT_H

#include <wayland-server-core.h>

struct server;
struct server_surface;
struct syn_key;

enum kb_modifier {
    KB_MOD_SHIFT = (1 << 0),
    KB_MOD_CAPS = (1 << 1),
    KB_MOD_CTRL = (1 << 2),
    KB_MOD_ALT = (1 << 3),
    KB_MOD_MOD2 = (1 << 4),
    KB_MOD_MOD3 = (1 << 5),
    KB_MOD_LOGO = (1 << 6),
    KB_MOD_MOD5 = (1 << 7),
};

struct kb_modifiers {
    uint8_t indices[8];
};

struct server_seat_g {
    struct wl_global *global;

    struct config *cfg;
    struct server *server;

    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct {
        struct {
            int32_t fd;
            uint32_t size;
        } local_keymap;

        struct {
            int32_t fd;
            uint32_t size;

            struct xkb_keymap *xkb;
            struct xkb_state *xkb_state;
            struct kb_modifiers mods;
        } remote_keymap;

        int32_t repeat_rate, repeat_delay;

        struct {
            uint32_t depressed, latched, locked;
            uint32_t group;
        } mods;
        struct {
            uint32_t *data;
            size_t cap, len;
        } pressed;
    } kb_state;
    struct {
        double x, y;
    } ptr_state;

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
        struct wl_signal pointer_enter; // data: uint32_t *
    } events;
};

struct server_seat_g *server_seat_g_create(struct server *server, struct config *cfg);
void server_seat_g_send_click(struct server_seat_g *seat_g, struct server_view *view);
void server_seat_g_send_keys(struct server_seat_g *seat_g, struct server_view *view,
                             const struct syn_key *keys, size_t num_keys);
void server_seat_g_set_listener(struct server_seat_g *seat_g,
                                const struct server_seat_listener *listener, void *data);

#endif
