#ifndef WAYWALL_COMPOSITOR_XDG_DECORATION_H
#define WAYWALL_COMPOSITOR_XDG_DECORATION_H

#include <wayland-server-core.h>

struct server;

struct server_xdg_decoration {
    struct wl_global *global;

    struct wl_listener display_destroy;
};

struct server_toplevel_decoration {
    struct server_xdg_toplevel *parent;

    struct wl_resource *resource;

    struct wl_listener on_destroy;
};

struct server_xdg_decoration *server_xdg_decoration_create(struct server *server);

struct server_toplevel_decoration *
server_toplevel_decoration_from_resource(struct wl_resource *resource);
struct server_xdg_decoration *server_xdg_decoration_from_resource(struct wl_resource *resource);

#endif
