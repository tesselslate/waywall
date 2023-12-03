#ifndef WAYWALL_COMPOSITOR_XDG_SHELL_H
#define WAYWALL_COMPOSITOR_XDG_SHELL_H

#include "compositor/ringbuf.h"
#include <wayland-server-core.h>

struct server;

struct server_xdg_wm_base {
    struct wl_global *global;

    struct wl_listener display_destroy;
};

struct server_xdg_surface {
    struct client_xdg_wm_base *parent;
    struct wl_resource *resource;

    struct server_surface *surface;
    struct server_xdg_toplevel *toplevel;

    struct ringbuf configure_serials;

    struct wl_listener on_commit;
};

struct server_xdg_toplevel {
    struct server_xdg_surface *parent;
    struct wl_resource *resource;

    char *title;

    struct {
        uint32_t width, height;
        bool fullscreen;
    } pending, current;
};

struct server_xdg_wm_base *server_xdg_wm_base_create(struct server *server);

#endif
