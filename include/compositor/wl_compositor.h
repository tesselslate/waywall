#ifndef WAYWALL_COMPOSITOR_WL_COMPOSITOR_H
#define WAYWALL_COMPOSITOR_WL_COMPOSITOR_H

#define COMPOSITOR_REMOTE_VERSION 4

#include <wayland-server-core.h>

struct server;

enum server_surface_role {
    ROLE_NONE,
    ROLE_CURSOR,   // no role object
    ROLE_TOPLEVEL, // xdg_toplevel resource (server_toplevel userdata)
};

struct server_compositor {
    struct wl_compositor *remote;
    struct wl_global *global;

    struct wl_listener display_destroy;
};

struct server_surface {
    struct server_compositor *parent;
    struct wl_surface *remote;

    struct wl_resource *resource;

    enum server_surface_role role;
    struct wl_resource *role_object;

    struct server_buffer *current_buffer, *pending_buffer;
    bool pending_buffer_changed;

    struct {
        struct wl_signal commit;
    } events;
};

struct server_region {
    struct wl_region *remote;
    struct wl_resource *resource;
};

struct server_compositor *server_compositor_create(struct server *server,
                                                   struct wl_compositor *remote);

struct server_compositor *server_compositor_from_resource(struct wl_resource *resource);
struct server_region *server_region_from_resource(struct wl_resource *resource);
struct server_surface *server_surface_from_resource(struct wl_resource *resource);

#endif
