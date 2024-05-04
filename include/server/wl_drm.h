#ifndef WAYWALL_SERVER_WL_DRM_H
#define WAYWALL_SERVER_WL_DRM_H

#include "server/server.h"
#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-util.h>

struct server_drm {
    struct wl_global *global;
    struct wl_list objects; // server_drm_client.link

    struct server *server;

    struct wl_listener on_display_destroy;
};

struct server_drm_client {
    struct wl_resource *resource;
    struct wl_drm *remote;
};

struct server_drm *server_drm_create(struct server *server);

#endif
