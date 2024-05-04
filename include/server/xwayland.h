#ifndef WAYWALL_SERVER_XWAYLAND_H
#define WAYWALL_SERVER_XWAYLAND_H

#ifndef WAYWALL_XWAYLAND
#error xwayland.h should only be included with Xwayland support enabled
#endif

#include <stdint.h>
#include <wayland-server-core.h>

struct server;

struct server_xwayland {
    struct server *server;
    struct server_xwayland_shell *shell;

    struct xserver *xserver;
    struct xwm *xwm;

    struct wl_listener on_display_destroy;
    struct wl_listener on_ready;

    struct {
        struct wl_signal ready;
    } events;
};

struct server_xwayland *server_xwayland_create(struct server *server,
                                               struct server_xwayland_shell *shell);

#endif
