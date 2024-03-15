#ifndef WAYWALL_SERVER_WL_SHM_H
#define WAYWALL_SERVER_WL_SHM_H

#include <wayland-server-core.h>

struct server;

struct server_shm_g {
    struct wl_global *global;
    struct wl_list objects; // wl_resource link

    struct wl_shm *remote;
    struct wl_array *formats;         // server_backend.shm_formats
    struct wl_listener on_shm_format; // server_backend.events.shm_format

    struct wl_listener on_display_destroy;
};

struct server_shm_pool {
    struct wl_resource *resource;

    struct wl_array *formats;
    struct wl_shm_pool *remote;
    int32_t fd, sz;
};

struct server_shm_g *server_shm_g_create(struct server *server);

#endif
