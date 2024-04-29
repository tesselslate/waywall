#ifndef WAYWALL_XWAYLAND_XWAYLAND_H
#define WAYWALL_XWAYLAND_XWAYLAND_H

#include <stdint.h>
#include <wayland-server-core.h>

struct server;

struct server_xwayland {
    struct server *server;

    struct xserver *xserver;

    struct wl_listener on_display_destroy;
    struct wl_listener on_ready;

    struct {
        struct wl_signal ready;
    } events;
};

struct server_xwayland *server_xwayland_create(struct server *server);

#endif
