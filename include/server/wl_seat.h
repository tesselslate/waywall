#ifndef WAYWALL_SERVER_WL_SEAT_H
#define WAYWALL_SERVER_WL_SEAT_H

#include <wayland-server-core.h>

struct server;

struct server_seat_g {
    struct wl_global *global;

    struct wl_seat *remote;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct {
        int32_t keymap_fd;
        uint32_t keymap_size;
        int32_t repeat_rate, repeat_delay;

        uint32_t mods_depressed, mods_latched, mods_locked, group;
        struct {
            uint32_t *data;
            size_t cap, len;
        } pressed;
    } kb_state;
    struct {
        double x, y;
    } ptr_state;
    struct server_surface *input_focus;

    struct wl_list keyboards; // wl_resource link
    struct wl_list pointers;  // wl_resource link

    struct wl_listener on_seat_caps;

    struct wl_listener on_display_destroy;

    struct {
        struct wl_signal pointer; // data: struct wl_pointer *
    } events;
};

struct server_seat_g *server_seat_g_create(struct server *server);
void server_seat_g_set_input_focus(struct server_seat_g *seat_g, struct server_surface *surface);

#endif
