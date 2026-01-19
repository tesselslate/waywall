#include "server/wp_linux_drm_syncobj.h"
#include "linux-drm-syncobj-v1-client-protocol.h"
#include "linux-drm-syncobj-v1-server-protocol.h"
#include "server/backend.h"
#include "server/buffer.h"
#include "server/server.h"
#include "server/surface.h"
#include "util/alloc.h"
#include "util/prelude.h"
#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <wayland-server-protocol.h>

static constexpr int SRV_LINUX_DRM_SYNCOBJ_VERSION = 1;

static void
set_and_ref_timeline(struct server_drm_syncobj_timeline **dst,
                     struct server_drm_syncobj_timeline *src) {
    if (*dst == src) {
        return;
    }

    if (*dst) {
        server_drm_syncobj_timeline_unref(*dst);
    }

    if (src) {
        *dst = server_drm_syncobj_timeline_ref(src);
    } else {
        *dst = nullptr;
    }
}

static void
syncobj_state_reset(struct server_drm_syncobj_surface_state *state) {
    if (state->acquire.timeline) {
        server_drm_syncobj_timeline_unref(state->acquire.timeline);
    }
    if (state->release.timeline) {
        server_drm_syncobj_timeline_unref(state->release.timeline);
    }

    *state = (struct server_drm_syncobj_surface_state){};
}

static bool
syncobj_surface_exists(struct server_drm_syncobj_manager *syncobj_manager,
                       struct server_surface *surface) {
    struct wl_resource *resource;
    wl_resource_for_each(resource, &syncobj_manager->surfaces) {
        struct server_drm_syncobj_surface *syncobj_surface = wl_resource_get_user_data(resource);

        if (syncobj_surface->parent == surface) {
            return true;
        }
    }

    return false;
}

static void
syncobj_timeline_destroy(struct server_drm_syncobj_timeline *timeline) {
    close(timeline->fd);
    free(timeline);
}

static void
on_surface_commit(struct wl_listener *listener, void *data) {
    struct server_drm_syncobj_surface *syncobj_surface =
        wl_container_of(listener, syncobj_surface, on_surface_commit);

    if (syncobj_surface->pending.present & SYNCOBJ_SURFACE_STATE_ACQUIRE) {
        wp_linux_drm_syncobj_surface_v1_set_acquire_point(
            syncobj_surface->remote, syncobj_surface->pending.acquire.timeline->remote,
            syncobj_surface->pending.acquire.point_hi, syncobj_surface->pending.acquire.point_lo);

        set_and_ref_timeline(&syncobj_surface->current.acquire.timeline,
                             syncobj_surface->pending.acquire.timeline);

        syncobj_surface->current.acquire.point_hi = syncobj_surface->pending.acquire.point_hi;
        syncobj_surface->current.acquire.point_lo = syncobj_surface->pending.acquire.point_lo;
    }

    if (syncobj_surface->pending.present & SYNCOBJ_SURFACE_STATE_RELEASE) {
        wp_linux_drm_syncobj_surface_v1_set_release_point(
            syncobj_surface->remote, syncobj_surface->pending.release.timeline->remote,
            syncobj_surface->pending.release.point_hi, syncobj_surface->pending.release.point_lo);

        set_and_ref_timeline(&syncobj_surface->current.release.timeline,
                             syncobj_surface->pending.release.timeline);

        syncobj_surface->current.release.point_hi = syncobj_surface->pending.release.point_hi;
        syncobj_surface->current.release.point_lo = syncobj_surface->pending.release.point_lo;
    }

    syncobj_state_reset(&syncobj_surface->pending);
}

static void
on_surface_destroy(struct wl_listener *listener, void *data) {
    struct server_drm_syncobj_surface *syncobj_surface =
        wl_container_of(listener, syncobj_surface, on_surface_destroy);

    syncobj_surface->parent = nullptr;
}

static void
drm_syncobj_surface_resource_destroy(struct wl_resource *resource) {
    struct server_drm_syncobj_surface *syncobj_surface = wl_resource_get_user_data(resource);

    syncobj_state_reset(&syncobj_surface->pending);
    syncobj_state_reset(&syncobj_surface->current);

    wp_linux_drm_syncobj_surface_v1_destroy(syncobj_surface->remote);

    wl_list_remove(&syncobj_surface->on_surface_commit.link);
    wl_list_remove(&syncobj_surface->on_surface_destroy.link);

    wl_list_remove(wl_resource_get_link(resource));

    free(syncobj_surface);
}

static void
drm_syncobj_surface_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
drm_syncobj_surface_set_acquire_point(struct wl_client *client, struct wl_resource *resource,
                                      struct wl_resource *timeline_resource, uint32_t point_hi,
                                      uint32_t point_lo) {
    struct server_drm_syncobj_surface *syncobj_surface = wl_resource_get_user_data(resource);

    if (!syncobj_surface->parent) {
        wl_resource_post_error(
            resource, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_SURFACE,
            "wl_surface associated with wp_linux_drm_syncobj_surface_v1 already destroyed");
        return;
    }

    struct server_drm_syncobj_timeline *syncobj_timeline =
        wl_resource_get_user_data(timeline_resource);

    set_and_ref_timeline(&syncobj_surface->pending.acquire.timeline, syncobj_timeline);

    syncobj_surface->pending.acquire.point_hi = point_hi;
    syncobj_surface->pending.acquire.point_lo = point_lo;

    syncobj_surface->pending.present |= SYNCOBJ_SURFACE_STATE_ACQUIRE;
}

static void
drm_syncobj_surface_set_release_point(struct wl_client *client, struct wl_resource *resource,
                                      struct wl_resource *timeline_resource, uint32_t point_hi,
                                      uint32_t point_lo) {
    struct server_drm_syncobj_surface *syncobj_surface = wl_resource_get_user_data(resource);

    if (!syncobj_surface->parent) {
        wl_resource_post_error(
            resource, WP_LINUX_DRM_SYNCOBJ_SURFACE_V1_ERROR_NO_SURFACE,
            "wl_surface associated with wp_linux_drm_syncobj_surface_v1 already destroyed");
        return;
    }

    struct server_drm_syncobj_timeline *syncobj_timeline =
        wl_resource_get_user_data(timeline_resource);

    set_and_ref_timeline(&syncobj_surface->pending.release.timeline, syncobj_timeline);

    syncobj_surface->pending.release.point_hi = point_hi;
    syncobj_surface->pending.release.point_lo = point_lo;

    syncobj_surface->pending.present |= SYNCOBJ_SURFACE_STATE_RELEASE;
}

static const struct wp_linux_drm_syncobj_surface_v1_interface drm_syncobj_surface_impl = {
    .destroy = drm_syncobj_surface_destroy,
    .set_acquire_point = drm_syncobj_surface_set_acquire_point,
    .set_release_point = drm_syncobj_surface_set_release_point,
};

static void
drm_syncobj_timeline_resource_destroy(struct wl_resource *resource) {
    struct server_drm_syncobj_timeline *syncobj_timeline = wl_resource_get_user_data(resource);

    wp_linux_drm_syncobj_timeline_v1_destroy(syncobj_timeline->remote);

    syncobj_timeline->resource = nullptr;
    syncobj_timeline->remote = nullptr;

    server_drm_syncobj_timeline_unref(syncobj_timeline);
}

static void
drm_syncobj_timeline_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct wp_linux_drm_syncobj_timeline_v1_interface drm_syncobj_timeline_impl = {
    .destroy = drm_syncobj_timeline_destroy,
};

static void
drm_syncobj_manager_resource_destroy(struct wl_resource *resource) {
    // Unused.
}

static void
drm_syncobj_manager_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
drm_syncobj_manager_get_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                                struct wl_resource *surface_resource) {
    struct server_drm_syncobj_manager *syncobj_manager = wl_resource_get_user_data(resource);
    struct server_surface *surface = server_surface_from_resource(surface_resource);

    if (syncobj_surface_exists(syncobj_manager, surface)) {
        wl_resource_post_error(resource, WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_SURFACE_EXISTS,
                               "wp_linux_drm_syncobj_surface_v1 already exists for given surface");
        return;
    }

    struct server_drm_syncobj_surface *syncobj_surface = zalloc(1, sizeof(*syncobj_surface));

    struct wl_resource *syncobj_surface_resource = wl_resource_create(
        client, &wp_linux_drm_syncobj_surface_v1_interface, wl_resource_get_version(resource), id);
    check_alloc(syncobj_surface_resource);
    wl_resource_set_implementation(syncobj_surface_resource, &drm_syncobj_surface_impl,
                                   syncobj_surface, drm_syncobj_surface_resource_destroy);

    syncobj_surface->resource = syncobj_surface_resource;
    syncobj_surface->manager = syncobj_manager;
    syncobj_surface->parent = surface;

    syncobj_surface->remote =
        wp_linux_drm_syncobj_manager_v1_get_surface(syncobj_manager->remote, surface->remote);
    check_alloc(syncobj_surface->remote);

    syncobj_state_reset(&syncobj_surface->pending);
    syncobj_state_reset(&syncobj_surface->current);

    syncobj_surface->on_surface_commit.notify = on_surface_commit;
    wl_signal_add(&surface->events.commit, &syncobj_surface->on_surface_commit);

    syncobj_surface->on_surface_destroy.notify = on_surface_destroy;
    wl_signal_add(&surface->events.destroy, &syncobj_surface->on_surface_destroy);

    wl_list_insert(&syncobj_manager->surfaces, wl_resource_get_link(syncobj_surface_resource));
}

static void
drm_syncobj_manager_import_timeline(struct wl_client *client, struct wl_resource *resource,
                                    uint32_t id, int32_t fd) {
    struct server_drm_syncobj_manager *syncobj_manager = wl_resource_get_user_data(resource);

    struct server_drm_syncobj_timeline *syncobj_timeline = zalloc(1, sizeof(*syncobj_timeline));
    syncobj_timeline->refcount = 1;

    struct wl_resource *syncobj_timeline_resource = wl_resource_create(
        client, &wp_linux_drm_syncobj_timeline_v1_interface, wl_resource_get_version(resource), id);
    check_alloc(syncobj_timeline_resource);
    wl_resource_set_implementation(syncobj_timeline_resource, &drm_syncobj_timeline_impl,
                                   syncobj_timeline, drm_syncobj_timeline_resource_destroy);

    syncobj_timeline->remote =
        wp_linux_drm_syncobj_manager_v1_import_timeline(syncobj_manager->remote, fd);
    check_alloc(syncobj_timeline->remote);

    syncobj_timeline->resource = syncobj_timeline_resource;
    syncobj_timeline->fd = fd;
}

static const struct wp_linux_drm_syncobj_manager_v1_interface drm_syncobj_manager_impl = {
    .destroy = drm_syncobj_manager_destroy,
    .get_surface = drm_syncobj_manager_get_surface,
    .import_timeline = drm_syncobj_manager_import_timeline,
};

static void
on_global_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SRV_LINUX_DRM_SYNCOBJ_VERSION);

    struct server_drm_syncobj_manager *syncobj_manager = data;

    struct wl_resource *resource =
        wl_resource_create(client, &wp_linux_drm_syncobj_manager_v1_interface, version, id);
    check_alloc(resource);
    wl_resource_set_implementation(resource, &drm_syncobj_manager_impl, syncobj_manager,
                                   drm_syncobj_manager_resource_destroy);
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_drm_syncobj_manager *syncobj_manager =
        wl_container_of(listener, syncobj_manager, on_display_destroy);

    wl_global_destroy(syncobj_manager->global);

    wl_list_remove(&syncobj_manager->on_display_destroy.link);

    free(syncobj_manager);
}

struct server_drm_syncobj_manager *
server_drm_syncobj_manager_create(struct server *server) {
    struct server_drm_syncobj_manager *syncobj_manager = zalloc(1, sizeof(*syncobj_manager));

    syncobj_manager->global =
        wl_global_create(server->display, &wp_linux_drm_syncobj_manager_v1_interface,
                         SRV_LINUX_DRM_SYNCOBJ_VERSION, syncobj_manager, on_global_bind);
    check_alloc(syncobj_manager->global);

    wl_list_init(&syncobj_manager->surfaces);
    syncobj_manager->remote = server->backend->linux_drm_syncobj_manager;

    syncobj_manager->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &syncobj_manager->on_display_destroy);

    return syncobj_manager;
}

struct server_drm_syncobj_timeline *
server_drm_syncobj_timeline_ref(struct server_drm_syncobj_timeline *timeline) {
    timeline->refcount++;

    return timeline;
}

void
server_drm_syncobj_timeline_unref(struct server_drm_syncobj_timeline *timeline) {
    timeline->refcount--;

    if (timeline->refcount == 0) {
        if (timeline->resource) {
            ww_panic("server_drm_syncobj_timeline with live resource has 0 refs");
        }

        syncobj_timeline_destroy(timeline);
    }
}
