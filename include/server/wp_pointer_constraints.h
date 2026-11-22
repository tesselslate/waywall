#pragma once

#include "config/config.h"
#include <wayland-server-core.h>
#include <wayland-util.h>

struct server_pointer_constraints {
    struct wl_global *global;
    struct wl_list obj_locked; // wl_resource (zwp_locked_pointer_v1 object) link

    struct {
        bool confine;
    } config;

    bool locked;
    // waywall lost keyboard focus while locked: the remote lock was released so the host
    // compositor puts the pointer at the hint (window centre); it's taken again on focus.
    bool suspended;
    struct {
        double x, y;
    } hint;

    struct server *server;
    struct server_view *input_focus;

    struct zwp_pointer_constraints_v1 *remote;
    struct zwp_confined_pointer_v1 *confined_pointer;
    struct zwp_locked_pointer_v1 *locked_pointer;

    struct wl_listener on_input_focus;
    struct wl_listener on_pointer;
    struct wl_listener on_keyboard_enter;
    struct wl_listener on_keyboard_leave;

    struct wl_listener on_display_destroy;
};

struct server_pointer_constraints *server_pointer_constraints_create(struct server *server,
                                                                     struct config *cfg);
void server_pointer_constraints_set_confine(struct server_pointer_constraints *pointer_constraints,
                                            bool confine);
void server_pointer_constraints_set_hint(struct server_pointer_constraints *pointer_constraints,
                                         double x, double y);
