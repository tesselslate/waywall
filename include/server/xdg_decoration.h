#pragma once

#include "server/server.h"
#include <wayland-server-core.h>
#include <wayland-util.h>

struct server_xdg_decoration_manager {
    struct wl_global *global;
    struct wl_list decorations; // server_xdg_toplevel_decoration.link

    struct wl_listener on_display_destroy;
};

struct server_xdg_toplevel_decoration {
    struct wl_list link; // server_xdg_decoration_manager.decorations
    struct wl_resource *resource;

    struct server_xdg_decoration_manager *parent;
    struct server_xdg_toplevel *xdg_toplevel;

    struct wl_listener on_toplevel_destroy;
};

struct server_xdg_decoration_manager *server_xdg_decoration_manager_create(struct server *server);
