#include "server/wl_compositor.h"
#include "server/buffer.h"
#include "server/server.h"
#include "util.h"
#include <stdlib.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server.h>

#define SRV_COMPOSITOR_VERSION 5

struct server_region_op {
    bool add;
    int32_t x, y, width, height;
};

struct server_surface_damage {
    int32_t x, y, width, height;
};

struct server_surface_frame {
    struct wl_resource *resource; // wl_callback

    struct server_surface *surface;
    struct wl_callback *remote;
};

static void
surface_frame_resource_destroy(struct wl_resource *resource) {
    struct server_surface_frame *frame = wl_resource_get_user_data(resource);

    wl_callback_destroy(frame->remote);
    free(frame);
}

static void
on_surface_frame_done(void *data, struct wl_callback *wl, uint32_t callback_data) {
    struct server_surface_frame *frame = data;

    wl_callback_send_done(frame->resource, callback_data);
}

static const struct wl_callback_listener surface_frame_listener = {
    .done = on_surface_frame_done,
};

static void
region_resource_destroy(struct wl_resource *resource) {
    struct server_region *region = wl_resource_get_user_data(resource);

    wl_region_destroy(region->remote);
    wl_array_release(&region->ops);
    free(region);
}

static void
region_add(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y,
           int32_t width, int32_t height) {
    struct server_region *region = wl_resource_get_user_data(resource);

    wl_region_add(region->remote, x, y, width, height);

    struct server_region_op *op = wl_array_add(&region->ops, sizeof(*op));
    ww_assert(op);
    op->add = true;
    op->x = x;
    op->y = y;
    op->width = width;
    op->height = height;
}

static void
region_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
region_subtract(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y,
                int32_t width, int32_t height) {
    struct server_region *region = wl_resource_get_user_data(resource);

    wl_region_subtract(region->remote, x, y, width, height);

    struct server_region_op *op = wl_array_add(&region->ops, sizeof(*op));
    ww_assert(op);
    op->add = false;
    op->x = x;
    op->y = y;
    op->width = width;
    op->height = height;
}

static const struct wl_region_interface region_impl = {
    .add = region_add,
    .destroy = region_destroy,
    .subtract = region_subtract,
};

static void
surface_resource_destroy(struct wl_resource *resource) {
    struct server_surface *surface = wl_resource_get_user_data(resource);

    wl_signal_emit_mutable(&surface->events.destroy, NULL);

    wl_surface_destroy(surface->remote);
    free(surface);
}

static void
surface_attach(struct wl_client *client, struct wl_resource *resource,
               struct wl_resource *buffer_resource, int32_t x, int32_t y) {
    struct server_surface *surface = wl_resource_get_user_data(resource);
    struct server_buffer *buffer = server_buffer_from_resource(buffer_resource);

    if (x != 0 || y != 0) {
        int version = wl_resource_get_version(resource);
        if (version >= WL_SURFACE_OFFSET_SINCE_VERSION) {
            wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_OFFSET,
                                   "non-zero offset provided to wl_surface.attach");
            return;
        } else {
            // Hopefully no relevant clients make use of this function. GLFW and Xserver don't seem
            // to, but Mesa makes some calls to `wl_surface_attach` that do not explicitly pass in
            // 0.
            wl_client_post_implementation_error(client,
                                                "non-zero offset provided to wl_surface.attach");
            return;
        }
    }

    surface->pending.buffer = buffer;
    surface->pending.apply |= SURFACE_STATE_ATTACH;
}

static void
surface_commit(struct wl_client *client, struct wl_resource *resource) {
    struct server_surface *surface = wl_resource_get_user_data(resource);

    struct server_surface_state *state = &surface->pending;
    wl_signal_emit_mutable(&surface->events.commit, state);

    if (state->apply & SURFACE_STATE_ATTACH) {
        // TODO: If the provided buffer's size is not a multiple of the buffer scale, the host
        // compositor will raise a protocol error (wl_surface.invalid_size) and kill us. Need to add
        // some functions to server_buffer that allow for getting width/height to check this.
        wl_surface_attach(surface->remote, state->buffer->remote, 0, 0);
        surface->current.buffer = state->buffer;
    }
    if (state->apply & SURFACE_STATE_DAMAGE) {
        struct server_surface_damage *dmg;
        wl_array_for_each(dmg, &state->damage) {
            wl_surface_damage(surface->remote, dmg->x, dmg->y, dmg->width, dmg->height);
        }
    }
    if (state->apply & SURFACE_STATE_DAMAGE_BUFFER) {
        struct server_surface_damage *dmg;
        wl_array_for_each(dmg, &state->buffer_damage) {
            wl_surface_damage_buffer(surface->remote, dmg->x, dmg->y, dmg->width, dmg->height);
        }
    }
    if (state->apply & SURFACE_STATE_SCALE) {
        wl_surface_set_buffer_scale(surface->remote, state->scale);
        surface->current.buffer_scale = state->scale;
    }
    if (state->apply & SURFACE_STATE_OPAQUE) {
        struct wl_region *region = wl_compositor_create_region(surface->parent->remote);
        ww_assert(region);

        struct server_region_op *op;
        wl_array_for_each(op, &state->opaque) {
            if (op->add) {
                wl_region_add(region, op->x, op->y, op->width, op->height);
            } else {
                wl_region_subtract(region, op->x, op->y, op->width, op->height);
            }
        }

        wl_surface_set_opaque_region(surface->remote, region);
        wl_region_destroy(region);
    }

    wl_array_release(&state->damage);
    wl_array_release(&state->buffer_damage);
    wl_array_release(&state->opaque);
    surface->pending = (struct server_surface_state){0};
    wl_array_init(&surface->pending.damage);
    wl_array_init(&surface->pending.buffer_damage);
    wl_array_init(&state->opaque);

    wl_surface_commit(surface->remote);
}

static void
surface_damage(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y,
               int32_t width, int32_t height) {
    struct server_surface *surface = wl_resource_get_user_data(resource);

    struct server_surface_damage *dmg = wl_array_add(&surface->pending.damage, sizeof(*dmg));
    ww_assert(dmg);
    dmg->x = x;
    dmg->y = y;
    dmg->width = width;
    dmg->height = height;
    surface->pending.apply |= SURFACE_STATE_DAMAGE;
}

static void
surface_damage_buffer(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y,
                      int32_t width, int32_t height) {
    struct server_surface *surface = wl_resource_get_user_data(resource);

    struct server_surface_damage *dmg = wl_array_add(&surface->pending.buffer_damage, sizeof(*dmg));
    ww_assert(dmg);
    dmg->x = x;
    dmg->y = y;
    dmg->width = width;
    dmg->height = height;
    surface->pending.apply |= SURFACE_STATE_DAMAGE_BUFFER;
}

static void
surface_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
surface_frame(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_surface *surface = wl_resource_get_user_data(resource);

    struct server_surface_frame *frame = calloc(1, sizeof(*frame));
    if (!frame) {
        wl_resource_post_no_memory(resource);
        return;
    }

    frame->resource = wl_resource_create(client, &wl_callback_interface, 1, id);
    if (!frame->resource) {
        free(frame);
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(frame->resource, NULL, frame, surface_frame_resource_destroy);

    frame->remote = wl_surface_frame(surface->remote);
    ww_assert(frame->remote);
    wl_callback_add_listener(frame->remote, &surface_frame_listener, frame);
}

static void
surface_offset(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y) {
    // This method appears to be unused by relevant clients (GLFW, Xserver, Mesa). We do not want
    // to support it anyway.
    wl_client_post_implementation_error(client, "wl_surface.offset is not supported");
}

static void
surface_set_buffer_scale(struct wl_client *client, struct wl_resource *resource, int32_t scale) {
    struct server_surface *surface = wl_resource_get_user_data(resource);

    if (scale <= 0) {
        wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_SCALE, "scale not positive");
        return;
    }

    surface->pending.scale = scale;
    surface->pending.apply |= SURFACE_STATE_SCALE;
}

static void
surface_set_buffer_transform(struct wl_client *client, struct wl_resource *resource,
                             int32_t transform) {
    // This method appears to be unused by relevant clients.
    wl_client_post_implementation_error(client, "wl_surface.set_buffer_transform is not supported");
}

static void
surface_set_input_region(struct wl_client *client, struct wl_resource *resource,
                         struct wl_resource *region_resource) {
    // Unused. The base Wayland protocol unfortunately leaves a lot of details regarding subsurfaces
    // unspecified, resulting in a lot of variation in behavior between compositors. In particular,
    // some compositors seem to pass certain input events through to the root surface, so we do not
    // want to ever receive input events on another surface.
}

static void
surface_set_opaque_region(struct wl_client *client, struct wl_resource *resource,
                          struct wl_resource *region_resource) {
    struct server_surface *surface = wl_resource_get_user_data(resource);
    struct server_region *region = wl_resource_get_user_data(region_resource);

    wl_array_copy(&surface->pending.opaque, &region->ops);
    surface->pending.apply |= SURFACE_STATE_OPAQUE;
}

static const struct wl_surface_interface surface_impl = {
    .attach = surface_attach,
    .commit = surface_commit,
    .damage = surface_damage,
    .damage_buffer = surface_damage_buffer,
    .destroy = surface_destroy,
    .frame = surface_frame,
    .offset = surface_offset,
    .set_buffer_scale = surface_set_buffer_scale,
    .set_buffer_transform = surface_set_buffer_transform,
    .set_input_region = surface_set_input_region,
    .set_opaque_region = surface_set_opaque_region,
};

static void
compositor_resource_destroy(struct wl_resource *resource) {
    struct server_compositor *compositor = wl_resource_get_user_data(resource);

    free(compositor);
}

static void
compositor_create_region(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_compositor *compositor = wl_resource_get_user_data(resource);

    struct server_region *region = calloc(1, sizeof(*region));
    if (!region) {
        wl_resource_post_no_memory(resource);
        return;
    }

    region->resource =
        wl_resource_create(client, &wl_region_interface, wl_resource_get_version(resource), id);
    if (!region->resource) {
        free(region);
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(region->resource, &region_impl, region, region_resource_destroy);

    region->remote = wl_compositor_create_region(compositor->remote);
    ww_assert(region->remote);

    wl_array_init(&region->ops);
}

static void
compositor_create_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_compositor *compositor = wl_resource_get_user_data(resource);

    struct server_surface *surface = calloc(1, sizeof(*surface));
    if (!surface) {
        wl_resource_post_no_memory(resource);
        return;
    }

    surface->resource =
        wl_resource_create(client, &wl_surface_interface, wl_resource_get_version(resource), id);
    if (!surface->resource) {
        free(surface);
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(surface->resource, &surface_impl, surface,
                                   surface_resource_destroy);

    surface->remote = wl_compositor_create_surface(compositor->remote);
    ww_assert(surface->remote);

    // We need to ensure that input events are never given to a child surface. See
    // `surface_set_input_region` for more details.
    struct wl_region *region = wl_compositor_create_region(compositor->remote);
    wl_surface_set_input_region(surface->remote, region);
    wl_region_destroy(region);

    surface->parent = compositor;
    surface->current.buffer_scale = 1;

    wl_signal_init(&surface->events.commit);
    wl_signal_init(&surface->events.destroy);
}

static const struct wl_compositor_interface compositor_impl = {
    .create_region = compositor_create_region,
    .create_surface = compositor_create_surface,
};

static void
on_global_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SRV_COMPOSITOR_VERSION);

    struct server_compositor_g *compositor_g = data;

    struct server_compositor *compositor = calloc(1, sizeof(*compositor));
    if (!compositor) {
        wl_client_post_no_memory(client);
        return;
    }

    compositor->resource = wl_resource_create(client, &wl_compositor_interface, version, id);
    if (!compositor->resource) {
        free(compositor);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(compositor->resource, &compositor_impl, compositor,
                                   compositor_resource_destroy);

    compositor->remote = compositor_g->remote;
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_compositor_g *compositor_g =
        wl_container_of(listener, compositor_g, on_display_destroy);

    wl_global_destroy(compositor_g->global);
    wl_compositor_destroy(compositor_g->remote);

    wl_list_remove(&compositor_g->on_display_destroy.link);

    free(compositor_g);
}

struct server_compositor_g *
server_compositor_g_create(struct server *server) {
    struct server_compositor_g *compositor_g = calloc(1, sizeof(*compositor_g));
    if (!compositor_g) {
        ww_log(LOG_ERROR, "failed to allocate server_compositor_g");
        return NULL;
    }

    compositor_g->global = wl_global_create(server->display, &wl_compositor_interface,
                                            SRV_COMPOSITOR_VERSION, compositor_g, on_global_bind);
    ww_assert(compositor_g->global);

    compositor_g->remote = server->backend.compositor;

    compositor_g->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &compositor_g->on_display_destroy);

    return compositor_g;
}

struct server_region *
server_region_from_resource(struct wl_resource *resource) {
    ww_assert(wl_resource_instance_of(resource, &wl_region_interface, &region_impl));
    return wl_resource_get_user_data(resource);
}

struct server_surface *
server_surface_from_resource(struct wl_resource *resource) {
    ww_assert(wl_resource_instance_of(resource, &wl_surface_interface, &surface_impl));
    return wl_resource_get_user_data(resource);
}
