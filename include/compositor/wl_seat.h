#ifndef WAYWALL_COMPOSITOR_WL_SEAT_H
#define WAYWALL_COMPOSITOR_WL_SEAT_H

#include <wayland-server-core.h>

struct server;

struct server_seat {
    struct remote_seat *remote;
    struct wl_global *global;

    struct wl_list pointers;  // server_pointer.link
    struct wl_list keyboards; // server_keyboard.link

    struct server_surface *input_focus;

    uint32_t keymap_format, keymap_size;
    int32_t keymap_fd;
    uint32_t pressed_keys[128];
    uint32_t num_pressed_keys;
    uint32_t mods_depressed, mods_latched, mods_locked, group;

    double pointer_x, pointer_y;

    struct wl_listener display_destroy;
};

struct server_pointer {
    struct server_seat *parent;
    struct wl_list link;

    struct wl_resource *resource;
};

struct server_keyboard {
    struct server_seat *parent;
    struct wl_list link;

    struct wl_resource *resource;
};

struct remote_seat {
    struct server_seat *parent;

    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;

    uint32_t last_pointer_enter;
};

struct server_seat *server_seat_create(struct server *server, struct wl_seat *remote);

// TODO: function to change remote seat

#endif
