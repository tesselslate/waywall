#include "compositor/wl_compositor.h"
#include "compositor/buffer.h"
#include "compositor/server.h"
#include "util.h"
#include <pixman-1/pixman.h>
#include <wayland-client.h>
#include <wayland-server.h>

/*
 *  Needed for GLFW:
 *
 *  wl_compositor.create_region
 *  wl_compositor.create_surface
 *
 *  wl_region.add
 *  wl_region.destroy
 *
 *  wl_surface.attach
 *  wl_surface.commit
 *  wl_surface.damage
 *  wl_surface.destroy
 *  wl_surface.set_buffer_scale
 *  wl_surface.set_opaque_region
 *
 *  Needed for Mesa:
 *
 *  wl_surface.attach
 *  wl_surface.commit
 *  wl_surface.damage
 *  wl_surface.damage_buffer
 *  wl_surface.destroy
 *  wl_surface.frame
 */

#define VERSION 5
#define REGION_VERSION 1

static void
on_frame_callback_done(void *data, struct wl_callback *wl_callback, uint32_t time) {
    struct wl_resource *callback_resource = data;
    wl_callback_send_done(callback_resource, time);
}

static const struct wl_callback_listener frame_callback_listener = {
    .done = on_frame_callback_done,
};

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_compositor *compositor = wl_container_of(listener, compositor, display_destroy);

    if (compositor->remote) {
        wl_compositor_destroy(compositor->remote);
    }
    wl_global_destroy(compositor->global);

    free(compositor);
}

static void
frame_callback_destroy(struct wl_resource *resource) {
    struct wl_callback *callback = wl_resource_get_user_data(resource);
    wl_callback_destroy(callback);
}

static void
handle_region_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_region_add(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y,
                  int32_t width, int32_t height) {
    struct server_region *region = wl_resource_get_user_data(resource);

    wl_region_add(region->remote, x, y, width, height);
}

static void
handle_region_subtract(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y,
                       int32_t width, int32_t height) {
    struct server_region *region = wl_resource_get_user_data(resource);

    wl_region_subtract(region->remote, x, y, width, height);
}

static void
region_destroy(struct wl_resource *resource) {
    struct server_region *region = wl_resource_get_user_data(resource);

    wl_region_destroy(region->remote);
    free(region);
}

static const struct wl_region_interface region_impl = {
    .destroy = handle_region_destroy,
    .add = handle_region_add,
    .subtract = handle_region_subtract,
};

static void
handle_surface_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_surface_attach(struct wl_client *client, struct wl_resource *resource,
                      struct wl_resource *buffer_resource, int32_t x, int32_t y) {
    struct server_surface *surface = wl_resource_get_user_data(resource);
    struct server_buffer *buffer = wl_resource_get_user_data(buffer_resource);

    if (x != 0 || y != 0) {
        if (wl_resource_get_version(resource) >= WL_SURFACE_OFFSET_SINCE_VERSION) {
            wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_OFFSET,
                                   "non-zero offset provided in wl_surface.attach");
        } else {
            wl_client_post_implementation_error(client, "non-zero surface offset not allowed");
        }
        return;
    }

    wl_surface_attach(surface->remote, buffer->remote, 0, 0);
    surface->pending_buffer = buffer;
    surface->pending_buffer_changed = true;
}

static void
handle_surface_damage(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y,
                      int32_t width, int32_t height) {
    struct server_surface *surface = wl_resource_get_user_data(resource);

    wl_surface_damage(surface->remote, x, y, width, height);
}

static void
handle_surface_frame(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_surface *surface = wl_resource_get_user_data(resource);

    struct wl_callback *callback = wl_surface_frame(surface->remote);

    struct wl_resource *callback_resource =
        wl_resource_create(client, &wl_callback_interface, 1, id);
    wl_resource_set_implementation(callback_resource, NULL, callback, frame_callback_destroy);

    wl_callback_add_listener(callback, &frame_callback_listener, callback_resource);
}

static void
handle_surface_set_opaque_region(struct wl_client *client, struct wl_resource *resource,
                                 struct wl_resource *region_resource) {
    struct server_surface *surface = wl_resource_get_user_data(resource);
    struct server_region *region = wl_resource_get_user_data(region_resource);

    wl_surface_set_opaque_region(surface->remote, region->remote);
}

static void
handle_surface_set_input_region(struct wl_client *client, struct wl_resource *resource,
                                struct wl_resource *region_resource) {
    // We do not want to give clients control over the input regions of their surfaces. We want
    // to disable input on all of their surfaces so that all input events are passed through to
    // the remote parent surface.

    // No relevant clients make use of this function.
    wl_client_post_implementation_error(client, "wl_surface.set_input_region is not implemented");
}

static void
handle_surface_commit(struct wl_client *client, struct wl_resource *resource) {
    struct server_surface *surface = wl_resource_get_user_data(resource);

    wl_signal_emit_mutable(&surface->events.commit, surface);

    wl_surface_commit(surface->remote);
    if (surface->pending_buffer_changed) {
        surface->current_buffer = surface->pending_buffer;
        surface->pending_buffer = NULL;
    }

    surface->pending_buffer_changed = false;
}

static void
handle_surface_set_buffer_transform(struct wl_client *client, struct wl_resource *resource,
                                    int32_t transform) {
    // No relevant clients make use of this function.
    wl_client_post_implementation_error(client,
                                        "wl_surface.set_buffer_transform is not implemented");
}

static void
handle_surface_set_buffer_scale(struct wl_client *client, struct wl_resource *resource,
                                int32_t scale) {
    struct server_surface *surface = wl_resource_get_user_data(resource);

    // TODO: check that buffer scale is legal (kill client early instead of getting killed by host
    // compositor)

    wl_surface_set_buffer_scale(surface->remote, scale);
}

static void
handle_surface_damage_buffer(struct wl_client *client, struct wl_resource *resource, int32_t x,
                             int32_t y, int32_t width, int32_t height) {
    struct server_surface *surface = wl_resource_get_user_data(resource);

    wl_surface_damage_buffer(surface->remote, x, y, width, height);
}

static void
handle_surface_offset(struct wl_client *client, struct wl_resource *resource, int32_t x,
                      int32_t y) {
    // No relevant clients make use of this function.
    wl_client_post_implementation_error(client, "wl_surface.offset is not implemented");
}

static void
surface_destroy(struct wl_resource *resource) {
    struct server_surface *surface = wl_resource_get_user_data(resource);

    // TODO: destroy signal (for role objects or other interested modules)
    if (surface->role_object) {
        wl_resource_post_error(resource, WL_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,
                               "destroyed wl_surface before associated role object");
    }

    wl_surface_destroy(surface->remote);
    free(surface);
}

static const struct wl_surface_interface surface_impl = {
    .destroy = handle_surface_destroy,
    .attach = handle_surface_attach,
    .damage = handle_surface_damage,
    .frame = handle_surface_frame,
    .set_opaque_region = handle_surface_set_opaque_region,
    .set_input_region = handle_surface_set_input_region,
    .commit = handle_surface_commit,
    .set_buffer_transform = handle_surface_set_buffer_transform,
    .set_buffer_scale = handle_surface_set_buffer_scale,
    .damage_buffer = handle_surface_damage_buffer,
    .offset = handle_surface_offset,
};

static void
handle_compositor_create_region(struct wl_client *client, struct wl_resource *resource,
                                uint32_t id) {
    struct server_compositor *compositor = wl_resource_get_user_data(resource);

    struct server_region *region = calloc(1, sizeof(*region));
    if (!region) {
        wl_client_post_no_memory(client);
        return;
    }

    region->remote = wl_compositor_create_region(compositor->remote);

    struct wl_resource *region_resource =
        wl_resource_create(client, &wl_region_interface, REGION_VERSION, id);
    wl_resource_set_implementation(region_resource, &region_impl, region, region_destroy);

    region->resource = region_resource;
}

static void
handle_compositor_create_surface(struct wl_client *client, struct wl_resource *resource,
                                 uint32_t id) {
    struct server_compositor *compositor = wl_resource_get_user_data(resource);

    struct server_surface *surface = calloc(1, sizeof(*surface));
    if (!surface) {
        wl_client_post_no_memory(client);
        return;
    }

    wl_signal_init(&surface->events.commit);

    surface->parent = compositor;
    surface->remote = wl_compositor_create_surface(compositor->remote);

    struct wl_resource *surface_resource =
        wl_resource_create(client, &wl_surface_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(surface_resource, &surface_impl, surface, surface_destroy);

    surface->resource = surface_resource;
}

static void
compositor_destroy(struct wl_resource *resource) {
    // Unused.
}

static const struct wl_compositor_interface compositor_impl = {
    .create_region = handle_compositor_create_region,
    .create_surface = handle_compositor_create_surface,
};

static void
handle_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= VERSION);
    struct server_compositor *compositor = data;

    struct wl_resource *resource =
        wl_resource_create(client, &wl_compositor_interface, version, id);
    wl_resource_set_implementation(resource, &compositor_impl, compositor, compositor_destroy);
}

struct server_compositor *
server_compositor_create(struct server *server, struct wl_compositor *remote) {
    struct server_compositor *compositor = calloc(1, sizeof(*compositor));
    if (!compositor) {
        LOG(LOG_ERROR, "failed to allocate server_compositor");
        return NULL;
    }

    compositor->remote = remote;
    compositor->global = wl_global_create(server->display, &wl_compositor_interface, VERSION,
                                          compositor, handle_bind);

    compositor->display_destroy.notify = on_display_destroy;

    wl_display_add_destroy_listener(server->display, &compositor->display_destroy);

    return compositor;
}
