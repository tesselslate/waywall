#ifndef WAYWALL_COMPOSITOR_WP_RELATIVE_POINTER_H
#define WAYWALL_COMPOSITOR_WP_RELATIVE_POINTER_H

#include <wayland-server-core.h>

struct server;
struct server_seat;

struct server_relative_pointer_manager {
    struct zwp_relative_pointer_manager_v1 *remote;
    struct server_seat *seat;

    struct wl_global *global;

    struct wl_listener display_destroy;
    struct wl_listener seat_destroy;
};

struct server_relative_pointer_manager *
server_relative_pointer_manager_create(struct server *server, struct server_seat *seat,
                                       struct zwp_relative_pointer_manager_v1 *remote);

struct server_relative_pointer_manager *
server_relative_pointer_manager_from_resource(struct wl_resource *resource);

#endif
