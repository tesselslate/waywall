#ifndef WAYWALL_SERVER_WL_COMPOSITOR_H
#define WAYWALL_SERVER_WL_COMPOSITOR_H

#include <wayland-server-core.h>

struct server;

struct server_compositor {
    struct wl_global *global;

    struct wl_compositor *remote;

    struct wl_listener on_display_destroy;
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

    const struct server_surface_role *role;
    struct wl_resource *role_resource;
};

struct server_surface_role {
    const char *name;

    void (*commit)(struct wl_resource *role_resource);
    void (*destroy)(struct wl_resource *role_resource);
};

struct server_compositor *server_compositor_create(struct server *server);
struct server_region *server_region_from_resource(struct wl_resource *resource);
struct server_surface *server_surface_from_resource(struct wl_resource *resource);
int server_surface_set_role(struct server_surface *surface, const struct server_surface_role *role,
                            struct wl_resource *role_resource);

#endif
