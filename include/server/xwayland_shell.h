#ifndef WAYWALL_SERVER_XWAYLAND_SHELL_H
#define WAYWALL_SERVER_XWAYLAND_SHELL_H

#include "server/server.h"
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-util.h>

struct server_xwayland_shell {
    struct wl_global *global;
    struct wl_resource *resource;

    bool bound;

    struct wl_listener on_display_destroy;

    struct {
        struct wl_signal new_surface; // data: struct server_xwayland_surface *
    } events;
};

struct server_xwayland_surface {
    struct wl_resource *resource;

    struct server_surface *parent;

    bool pending_association;
    uint64_t pending_serial;
    bool associated;

    struct {
        struct wl_signal destroy;    // data: NULL
        struct wl_signal set_serial; // data: uint64_t *
    } events;
};

struct server_xwayland_shell *server_xwayland_shell_create(struct server *server);

#endif
