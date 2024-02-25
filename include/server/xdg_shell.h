#ifndef WAYWALL_SERVER_XDG_SHELL_H
#define WAYWALL_SERVER_XDG_SHELL_H

#include "server/serial.h"
#include <wayland-server-core.h>

struct server;

struct server_xdg_wm_base_g {
    struct wl_global *global;

    struct wl_listener on_display_destroy;
};

struct server_xdg_wm_base {
    struct wl_resource *resource;

    struct wl_list surfaces; // server_xdg_surface.link
};

struct server_xdg_surface {
    struct wl_list link; // server_xdg_wm_base.surfaces
    struct wl_resource *resource;

    struct server_xdg_wm_base *xdg_wm_base;
    struct server_surface *parent;
    struct server_xdg_toplevel *child;
    struct serial_ring serials;
    bool initial_commit;
    bool initial_ack;

    struct wl_listener on_commit;
    struct wl_listener on_destroy;
};

struct server_xdg_toplevel {
    struct wl_resource *resource;

    struct server_xdg_surface *parent;
    char *title;
    int32_t width, height;
};

struct server_xdg_wm_base_g *server_xdg_wm_base_g_create(struct server *server);
struct server_xdg_surface *server_xdg_surface_from_resource(struct wl_resource *resource);
struct server_xdg_toplevel *server_xdg_toplevel_from_resource(struct wl_resource *resource);

#endif
