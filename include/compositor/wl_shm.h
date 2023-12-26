#ifndef WAYWALL_COMPOSITOR_WL_SHM_H
#define WAYWALL_COMPOSITOR_WL_SHM_H

#define SHM_REMOTE_VERSION 1

#include <wayland-server-core.h>

struct server;

struct server_shm {
    struct wl_shm *remote;
    struct wl_global *global;

    struct wl_array formats;

    struct wl_listener display_destroy;
};

struct server_shm *server_shm_create(struct server *server, struct wl_shm *remote);

struct server_shm *server_shm_from_resource(struct wl_resource *resource);

#endif
