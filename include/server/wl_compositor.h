#ifndef WAYWALL_SERVER_WL_COMPOSITOR_H
#define WAYWALL_SERVER_WL_COMPOSITOR_H

#include <wayland-server-core.h>

struct server;

struct server_compositor_g {
    struct wl_global *global;

    struct wl_compositor *remote;

    struct wl_listener on_display_destroy;
};

struct server_compositor {
    struct wl_resource *resource;

    struct wl_compositor *remote;
};

struct server_region {
    struct wl_resource *resource;

    struct wl_region *remote;
    struct wl_array ops; // data: struct server_region_op
};

struct server_surface {
    struct wl_resource *resource;

    struct server_compositor *parent;
    struct wl_surface *remote;

    struct {
        struct server_buffer *buffer;
        int32_t buffer_scale;
    } current;

    struct server_surface_state {
        struct server_buffer *buffer;
        struct wl_array damage, buffer_damage; // data: struct server_surface_damage
        int32_t scale;
        struct wl_array opaque; // data: struct server_region_op

        enum {
            SURFACE_STATE_ATTACH = (1 << 0),
            SURFACE_STATE_DAMAGE = (1 << 1),
            SURFACE_STATE_DAMAGE_BUFFER = (1 << 2),
            SURFACE_STATE_SCALE = (1 << 3),
            SURFACE_STATE_OPAQUE = (1 << 4),
        } apply;
    } pending;

    enum server_surface_role {
        SURFACE_ROLE_NONE,
        SURFACE_ROLE_CURSOR,
        SURFACE_ROLE_XDG,
    } role;
    void *role_object;

    struct {
        struct wl_signal commit;  // data: struct server_surface_state *
        struct wl_signal destroy; // data: NULL
    } events;
};

struct server_compositor_g *server_compositor_g_create(struct server *server);
struct server_region *server_region_from_resource(struct wl_resource *resource);
struct server_surface *server_surface_from_resource(struct wl_resource *resource);

#endif
