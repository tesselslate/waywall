#pragma once

#include "server/server.h"
#include <wayland-server-core.h>
#include <wayland-util.h>

struct server_compositor {
    struct wl_global *global;

    struct wl_compositor *remote;
    struct wl_region *empty_region;

    struct wl_listener on_display_destroy;

    struct {
        struct wl_signal destroy;
        struct wl_signal new_surface;
    } events;
};

struct server_region {
    struct wl_resource *resource;

    struct wl_region *remote;
};

struct server_compositor *server_compositor_create(struct server *server);
struct server_region *server_region_from_resource(struct wl_resource *resource);
