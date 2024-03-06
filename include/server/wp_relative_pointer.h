#ifndef WAYWALL_SERVER_WP_RELATIVE_POINTER_H
#define WAYWALL_SERVER_WP_RELATIVE_POINTER_H

#include <wayland-server-core.h>

struct server;

struct server_relative_pointer_g {
    struct wl_global *global;
    struct wl_list objects; // wl_resource (zwp_relative_pointer_v1) link

    struct server_seat_g *seat_g;

    struct zwp_relative_pointer_manager_v1 *remote;
    struct zwp_relative_pointer_v1 *remote_pointer;

    struct wl_listener on_pointer;

    struct wl_listener on_display_destroy;
};

struct server_relative_pointer_g *server_relative_pointer_g_create(struct server *server);

#endif
