#ifndef WAYWALL_SERVER_WP_POINTER_CONSTRAINTS_H
#define WAYWALL_SERVER_WP_POINTER_CONSTRAINTS_H

#include <wayland-server-core.h>

struct server;

struct server_pointer_constraints {
    struct wl_global *global;
    struct wl_list objects; // wl_resource (zwp_locked_pointer_v1 object) link

    struct config *cfg;
    struct server *server;
    struct server_view *input_focus;

    struct zwp_pointer_constraints_v1 *remote;
    struct zwp_confined_pointer_v1 *confined_pointer;
    struct zwp_locked_pointer_v1 *locked_pointer;

    struct wl_listener on_input_focus;
    struct wl_listener on_pointer;

    struct wl_listener on_display_destroy;
};

struct server_pointer_constraints *server_pointer_constraints_create(struct server *server,
                                                                     struct config *cfg);
void server_pointer_constraints_set_hint(struct server_pointer_constraints *pointer_constraints,
                                         double x, double y);

#endif
