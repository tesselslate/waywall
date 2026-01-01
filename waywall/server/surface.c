#include "server/surface.h"
#include "server/buffer.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include <stdint.h>
#include <stdlib.h>
#include <wayland-client.h>
#include <wayland-server.h>

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
surface_state_reset(struct server_surface_state *state) {
    wl_array_release(&state->damage);
    wl_array_release(&state->buffer_damage);
    *state = (struct server_surface_state){};

    wl_array_init(&state->damage);
    wl_array_init(&state->buffer_damage);
}

static void
surface_resource_destroy(struct wl_resource *resource) {
    struct server_surface *surface = wl_resource_get_user_data(resource);

    wl_signal_emit_mutable(&surface->events.destroy, surface);

    if (surface->role && surface->role_resource) {
        surface->role->destroy(surface->role_resource);
    }

    if (surface->pending.buffer) {
        server_buffer_unref(surface->pending.buffer);
    }
    if (surface->current.buffer) {
        server_buffer_unref(surface->current.buffer);
    }

    wl_surface_destroy(surface->remote);
    free(surface);
}

static void
surface_attach(struct wl_client *client, struct wl_resource *resource,
               struct wl_resource *buffer_resource, int32_t x, int32_t y) {
    struct server_surface *surface = wl_resource_get_user_data(resource);
    struct server_buffer *buffer = server_buffer_from_resource(buffer_resource);

    // If a nullptr buffer (no buffer) is being attached, skip the checks.
    if (!buffer) {
        if (surface->pending.buffer) {
            server_buffer_unref(surface->pending.buffer);
        }

        surface->pending.buffer = nullptr;
        surface->pending.present |= SURFACE_STATE_BUFFER;
        return;
    }

    // We don't want offset surfaces in general, but we need to check which type of error to send.
    if (x != 0 || y != 0) {
        int version = wl_resource_get_version(resource);
        if (version >= WL_SURFACE_OFFSET_SINCE_VERSION) {
            wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_OFFSET,
                                   "non-zero offset provided to wl_surface.attach");
            return;
        } else {
            wl_client_post_implementation_error(client,
                                                "non-zero offset provided to wl_surface.attach");
            return;
        }
    }

    if (surface->pending.buffer) {
        server_buffer_unref(surface->pending.buffer);
    }
    server_buffer_ref(buffer);

    surface->pending.buffer = buffer;
    surface->pending.present |= SURFACE_STATE_BUFFER;
}

static void
surface_commit(struct wl_client *client, struct wl_resource *resource) {
    struct server_surface *surface = wl_resource_get_user_data(resource);

    if (surface->role && surface->role_resource) {
        surface->role->commit(surface->role_resource);
    }

    wl_signal_emit_mutable(&surface->events.commit, surface);

    if (surface->pending.present & SURFACE_STATE_BUFFER) {
        wl_surface_attach(surface->remote,
                          surface->pending.buffer ? surface->pending.buffer->remote : nullptr, 0,
                          0);

        if (surface->current.buffer) {
            server_buffer_unref(surface->current.buffer);
        }
        surface->current.buffer = surface->pending.buffer;

        server_buffer_lock(surface->current.buffer);
    }
    if (surface->pending.present & SURFACE_STATE_DAMAGE) {
        struct server_surface_damage *dmg;
        wl_array_for_each(dmg, &surface->pending.damage) {
            wl_surface_damage(surface->remote, dmg->x, dmg->y, dmg->width, dmg->height);
        }
    }
    if (surface->pending.present & SURFACE_STATE_DAMAGE_BUFFER) {
        struct server_surface_damage *dmg;
        wl_array_for_each(dmg, &surface->pending.buffer_damage) {
            wl_surface_damage_buffer(surface->remote, dmg->x, dmg->y, dmg->width, dmg->height);
        }
    }

    surface_state_reset(&surface->pending);
    wl_surface_commit(surface->remote);
}

static void
surface_damage(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y,
               int32_t width, int32_t height) {
    struct server_surface *surface = wl_resource_get_user_data(resource);

    struct server_surface_damage *dmg = wl_array_add(&surface->pending.damage, sizeof(*dmg));
    check_alloc(dmg);

    dmg->x = x;
    dmg->y = y;
    dmg->width = width;
    dmg->height = height;
    surface->pending.present |= SURFACE_STATE_DAMAGE;
}

static void
surface_damage_buffer(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y,
                      int32_t width, int32_t height) {
    struct server_surface *surface = wl_resource_get_user_data(resource);

    struct server_surface_damage *dmg = wl_array_add(&surface->pending.buffer_damage, sizeof(*dmg));
    check_alloc(dmg);

    dmg->x = x;
    dmg->y = y;
    dmg->width = width;
    dmg->height = height;
    surface->pending.present |= SURFACE_STATE_DAMAGE_BUFFER;
}

static void
surface_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
surface_frame(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_surface *surface = wl_resource_get_user_data(resource);

    struct server_surface_frame *frame = zalloc(1, sizeof(*frame));

    frame->resource = wl_resource_create(client, &wl_callback_interface, 1, id);
    check_alloc(frame->resource);
    wl_resource_set_implementation(frame->resource, nullptr, frame, surface_frame_resource_destroy);

    frame->remote = wl_surface_frame(surface->remote);
    check_alloc(frame->remote);
    wl_callback_add_listener(frame->remote, &surface_frame_listener, frame);
}

static void
surface_offset(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y) {
    // The NVIDIA driver seems to use this for some reason, so we need to allow calling it.
    if (x != 0 || y != 0) {
        ww_log(LOG_WARN, "wl_surface.offset called with non-zero offset");
    }
}

static void
surface_set_buffer_scale(struct wl_client *client, struct wl_resource *resource, int32_t scale) {
    struct server_surface *surface = wl_resource_get_user_data(resource);

    if (scale <= 0) {
        wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_SCALE, "scale not positive");
        return;
    }

    // TODO: Properly support buffer scaling (including fractional scaling, probably, maybe?)
    // As of right now, it's not actually supported, so there's no point pretending it is.

    if (scale != 1) {
        ww_log(LOG_WARN, "non-default buffer scale (%" PRIi32 " for surface %p)", scale, surface);
    }
}

static void
surface_set_buffer_transform(struct wl_client *client, struct wl_resource *resource,
                             int32_t transform) {
    // It appears that the userspace NVIDIA driver calls wl_surface.set_buffer_transform for
    // some reason, so we cannot kill the client for using it.
    if (transform != WL_OUTPUT_TRANSFORM_NORMAL) {
        ww_log(LOG_WARN, "client requested non-normal buffer transform");
    }
}

static void
surface_set_input_region(struct wl_client *client, struct wl_resource *resource,
                         struct wl_resource *region_resource) {
    // Unused. We want to ensure that all client surfaces have an empty input region so that all
    // input events in the waywall toplevel are passed to the root surface.
}

static void
surface_set_opaque_region(struct wl_client *client, struct wl_resource *resource,
                          struct wl_resource *region_resource) {
    // Unused. `set_opaque_region` is just an optimization hint that clients can give to the
    // compositor, but it requires additional bookkeeping code from waywall.
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

struct server_surface *
server_surface_create(struct wl_resource *resource) {
    struct server_surface *surface = zalloc(1, sizeof(*surface));

    surface->resource = resource;
    wl_resource_set_implementation(resource, &surface_impl, surface, surface_resource_destroy);

    return surface;
}

struct server_surface *
server_surface_from_resource(struct wl_resource *resource) {
    ww_assert(wl_resource_instance_of(resource, &wl_surface_interface, &surface_impl));
    return wl_resource_get_user_data(resource);
}

struct server_surface *
server_surface_try_from_resource(struct wl_resource *resource) {
    if (wl_resource_instance_of(resource, &wl_surface_interface, &surface_impl)) {
        return wl_resource_get_user_data(resource);
    }
    return nullptr;
}

struct server_buffer *
server_surface_next_buffer(struct server_surface *surface) {
    return (surface->pending.present & SURFACE_STATE_BUFFER) ? surface->pending.buffer
                                                             : surface->current.buffer;
}

int
server_surface_set_role(struct server_surface *surface, const struct server_surface_role *role,
                        struct wl_resource *role_resource) {
    if (surface->role && surface->role != role) {
        return 1;
    }
    if (surface->role_resource && role_resource) {
        return 1;
    }

    surface->role = role;
    surface->role_resource = role_resource;
    return 0;
}
