#pragma once

#include "server/server.h"
#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-util.h>

struct server_drm_syncobj_manager {
    struct wl_global *global;
    struct wl_list surfaces; // wl_resource link

    struct wp_linux_drm_syncobj_manager_v1 *remote;

    struct wl_listener on_display_destroy;
};

struct server_drm_syncobj_surface {
    struct wl_resource *resource;
    struct server_drm_syncobj_manager *manager;

    struct server_surface *parent;
    struct wp_linux_drm_syncobj_surface_v1 *remote;

    struct wl_listener on_surface_commit;
    struct wl_listener on_surface_destroy;

    struct server_drm_syncobj_surface_state {
        struct server_drm_syncobj_point {
            struct server_drm_syncobj_timeline *timeline;
            uint32_t point_hi, point_lo;
        } acquire, release;

        enum {
            SYNCOBJ_SURFACE_STATE_ACQUIRE = (1 << 1),
            SYNCOBJ_SURFACE_STATE_RELEASE = (1 << 2),
        } present;
    } current, pending;
};

struct server_drm_syncobj_timeline {
    struct wl_resource *resource;
    struct wp_linux_drm_syncobj_timeline_v1 *remote;

    int refcount;

    int32_t fd;
};

struct server_drm_syncobj_manager *server_drm_syncobj_manager_create(struct server *server);
struct server_drm_syncobj_timeline *
server_drm_syncobj_timeline_ref(struct server_drm_syncobj_timeline *timeline);
void server_drm_syncobj_timeline_unref(struct server_drm_syncobj_timeline *timeline);
