#pragma once

#include "server/server.h"
#include <stdint.h>
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

struct server_surface {
    struct wl_resource *resource;

    struct server_compositor *parent;
    struct wl_surface *remote;

    struct server_surface_state {
        struct server_buffer *buffer;
        struct wl_array damage, buffer_damage; // data: struct server_surface_damage

        enum {
            SURFACE_STATE_BUFFER = (1 << 0),
            SURFACE_STATE_DAMAGE = (1 << 1),
            SURFACE_STATE_DAMAGE_BUFFER = (1 << 2),
        } present;
    } current, pending;

    const struct server_surface_role *role;
    struct wl_resource *role_resource;

    struct {
        struct wl_signal commit;  // data: struct server_surface *
        struct wl_signal destroy; // data: struct server_surface *
    } events;
};

struct server_surface_role {
    const char *name;

    void (*commit)(struct wl_resource *role_resource);
    void (*destroy)(struct wl_resource *role_resource);
};

struct server_compositor *server_compositor_create(struct server *server);
struct server_region *server_region_from_resource(struct wl_resource *resource);
struct server_surface *server_surface_from_resource(struct wl_resource *resource);
struct server_surface *server_surface_try_from_resource(struct wl_resource *resource);

struct server_buffer *server_surface_next_buffer(struct server_surface *surface);
int server_surface_set_role(struct server_surface *surface, const struct server_surface_role *role,
                            struct wl_resource *role_resource);
