#ifndef WAYWALL_SERVER_XDG_DECORATION_H
#define WAYWALL_SERVER_XDG_DECORATION_H

#include <wayland-server-core.h>

struct server;

struct server_xdg_decoration_manager_g {
    struct wl_global *global;
    struct wl_list decorations; // server_xdg_toplevel_decoration.link

    struct wl_listener on_display_destroy;
};

struct server_xdg_toplevel_decoration {
    struct wl_list link; // server_xdg_decoration_manager_g.decorations
    struct wl_resource *resource;

    struct server_xdg_decoration_manager_g *parent;
    struct server_xdg_toplevel *xdg_toplevel;

    struct wl_listener on_toplevel_destroy;
};

struct server_xdg_decoration_manager_g *
server_xdg_decoration_manager_g_create(struct server *server);

#endif
