#ifndef WAYWALL_COMPOSITOR_WL_SEAT_H
#define WAYWALL_COMPOSITOR_WL_SEAT_H

#include "compositor/cutil.h"
#include <wayland-server-core.h>

#define MAX_PRESSED_KEYS 127
#define WL_SEAT_REMOTE_VERSION 5

struct server;

struct server_seat {
    struct wl_global *global;

    struct wl_list pointers;  // server_pointer.link
    struct wl_list keyboards; // server_keyboard.link

    struct server_view *input_focus;
    struct wl_listener input_focus_destroy;

    struct {
        uint32_t keymap_format, keymap_size;
        int32_t keymap_fd;
        struct u32_array keys;
        uint32_t mods_depressed, mods_latched, mods_locked, group;

        uint32_t last_pointer_enter;
        struct u32_array buttons;
        double pointer_x, pointer_y;

        struct wl_seat *seat;
        struct wl_keyboard *keyboard;
        struct wl_pointer *pointer;
    } remote;

    struct wl_listener display_destroy;

    struct {
        struct wl_signal destroy; // data: server_seat
    } events;
};

struct server_seat *server_seat_create(struct server *server);
void server_seat_set_input_focus(struct server_seat *seat, struct server_view *view);
void server_seat_set_caps(struct server_seat *seat, uint32_t caps);
void server_seat_set_remote(struct server_seat *seat, struct wl_seat *remote);

/*
 *  This function can take any of the following resources:
 *    - wl_seat
 *    - wl_pointer
 *    - wl_keyboard
 */
struct server_seat *server_seat_from_resource(struct wl_resource *resource);

#endif
