#include "compositor/wp_linux_dmabuf.h"
#include "compositor/server.h"
#include "compositor/wl_compositor.h"
#include "util.h"
#include <wayland-client.h>
#include <wayland-server.h>

#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"

/*
 *  Mesa uses a bunch of these functions. It's easy enough to just implement all of them since
 *  they're largely passthrough.
 */

#define VERSION 4

// TODO: Figure out how to safely set up and use a separate event queue for where
// wl_display_roundtrip is used.

struct server_buffer_params {
    struct server_linux_dmabuf *parent;

    struct wl_resource *resource;
    struct zwp_linux_buffer_params_v1 *remote;

    struct buffer_data_dmabuf data;
    uint8_t plane_bitmask;

    bool failed, created;
    struct wl_resource *buffer_resource;
};

struct server_dmabuf_feedback {
    struct wl_resource *resource;
    struct zwp_linux_dmabuf_feedback_v1 *remote;
};

static bool check_buffer_params_creation(struct server_buffer_params *buffer_params);
static void create_buffer(struct server_buffer_params *buffer_params,
                          struct wl_buffer *remote_buffer, struct wl_resource *buffer_resource);

static void
on_buffer_params_created(void *data, struct zwp_linux_buffer_params_v1 *wp_buffer_params,
                         struct wl_buffer *remote_buffer) {
    struct server_buffer_params *buffer_params = data;
    struct wl_client *client = wl_resource_get_client(buffer_params->resource);

    struct wl_resource *buffer_resource = wl_resource_create(client, &wl_buffer_interface, 1, 0);
    if (!buffer_resource) {
        wl_client_post_no_memory(client);
        return;
    }

    create_buffer(buffer_params, remote_buffer, buffer_resource);
    zwp_linux_buffer_params_v1_send_created(buffer_params->resource, buffer_resource);
}

static void
on_buffer_params_failed(void *data, struct zwp_linux_buffer_params_v1 *wp_buffer_params) {
    struct server_buffer_params *buffer_params = data;

    buffer_params->failed = true;
    zwp_linux_buffer_params_v1_send_failed(buffer_params->resource);
}

static const struct zwp_linux_buffer_params_v1_listener buffer_params_listener = {
    .created = on_buffer_params_created,
    .failed = on_buffer_params_failed,
};

static void
on_dmabuf_feedback_done(void *data, struct zwp_linux_dmabuf_feedback_v1 *wp_dmabuf_feedback) {
    struct server_dmabuf_feedback *dmabuf_feedback = data;

    zwp_linux_dmabuf_feedback_v1_send_done(dmabuf_feedback->resource);
}

static void
on_dmabuf_feedback_format_table(void *data, struct zwp_linux_dmabuf_feedback_v1 *wp_dmabuf_feedback,
                                int32_t fd, uint32_t size) {
    struct server_dmabuf_feedback *dmabuf_feedback = data;

    zwp_linux_dmabuf_feedback_v1_send_format_table(dmabuf_feedback->resource, fd, size);
    close(fd);
}

static void
on_dmabuf_feedback_main_device(void *data, struct zwp_linux_dmabuf_feedback_v1 *wp_dmabuf_feedback,
                               struct wl_array *device) {
    struct server_dmabuf_feedback *dmabuf_feedback = data;

    zwp_linux_dmabuf_feedback_v1_send_main_device(dmabuf_feedback->resource, device);
}

static void
on_dmabuf_feedback_tranche_done(void *data,
                                struct zwp_linux_dmabuf_feedback_v1 *wp_dmabuf_feedback) {
    struct server_dmabuf_feedback *dmabuf_feedback = data;

    zwp_linux_dmabuf_feedback_v1_send_tranche_done(dmabuf_feedback->resource);
}

static void
on_dmabuf_feedback_tranche_target_device(void *data,
                                         struct zwp_linux_dmabuf_feedback_v1 *wp_dmabuf_feedback,
                                         struct wl_array *device) {
    struct server_dmabuf_feedback *dmabuf_feedback = data;

    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(dmabuf_feedback->resource, device);
}

static void
on_dmabuf_feedback_tranche_formats(void *data,
                                   struct zwp_linux_dmabuf_feedback_v1 *wp_dmabuf_feedback,
                                   struct wl_array *indices) {
    struct server_dmabuf_feedback *dmabuf_feedback = data;

    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(dmabuf_feedback->resource, indices);
}

static void
on_dmabuf_feedback_tranche_flags(void *data,
                                 struct zwp_linux_dmabuf_feedback_v1 *wp_dmabuf_feedback,
                                 uint32_t flags) {
    struct server_dmabuf_feedback *dmabuf_feedback = data;

    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(dmabuf_feedback->resource, flags);
}

static const struct zwp_linux_dmabuf_feedback_v1_listener dmabuf_feedback_listener = {
    .done = on_dmabuf_feedback_done,
    .format_table = on_dmabuf_feedback_format_table,
    .main_device = on_dmabuf_feedback_main_device,
    .tranche_done = on_dmabuf_feedback_tranche_done,
    .tranche_target_device = on_dmabuf_feedback_tranche_target_device,
    .tranche_formats = on_dmabuf_feedback_tranche_formats,
    .tranche_flags = on_dmabuf_feedback_tranche_flags,
};

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_linux_dmabuf *linux_dmabuf =
        wl_container_of(listener, linux_dmabuf, display_destroy);

    if (linux_dmabuf->remote) {
        zwp_linux_dmabuf_v1_destroy(linux_dmabuf->remote);
    }
    wl_global_destroy(linux_dmabuf->global);

    free(linux_dmabuf);
}

static void
server_buffer_dmabuf_destroy(struct wl_resource *resource) {
    struct server_buffer *buffer = server_buffer_from_resource(resource);

    server_buffer_destroy(buffer);
}

static bool
check_buffer_params_creation(struct server_buffer_params *buffer_params) {
    struct wl_resource *resource = buffer_params->resource;

    if (buffer_params->created) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                               "already issued create for this wp_linux_buffer_params");
        return false;
    }

    if (buffer_params->data.num_planes == 0) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE, "no planes");
        return false;
    }

    uint8_t expected_mask = (2 << buffer_params->data.num_planes) - 1;
    if (expected_mask != buffer_params->plane_bitmask) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
                               "gap in planes");
        return false;
    }

    return true;
}

static void
create_buffer(struct server_buffer_params *buffer_params, struct wl_buffer *remote_buffer,
              struct wl_resource *buffer_resource) {
    struct wl_client *client = wl_resource_get_client(buffer_params->resource);

    struct server_buffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) {
        wl_buffer_destroy(remote_buffer);
        wl_client_post_no_memory(client);
        return;
    }

    wl_buffer_add_listener(remote_buffer, &server_buffer_listener, buffer);

    wl_resource_set_implementation(buffer_resource, &server_buffer_impl, buffer,
                                   server_buffer_dmabuf_destroy);

    buffer->resource = buffer_resource;
    buffer->remote = remote_buffer;
    buffer->type = BUFFER_DMABUF;
    buffer->data.dmabuf = buffer_params->data;
}

static void
handle_buffer_params_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_buffer_params_add(struct wl_client *client, struct wl_resource *resource, int32_t fd,
                         uint32_t plane_idx, uint32_t offset, uint32_t stride, uint32_t modifier_hi,
                         uint32_t modifier_lo) {
    struct server_buffer_params *buffer_params = wl_resource_get_user_data(resource);

    if (buffer_params->created) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                               "already issued create for this wp_linux_buffer_params");
        return;
    }

    if (buffer_params->data.num_planes == MAX_PLANES) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
                               "too many planes added to buffer parameters");
        return;
    }

    if (plane_idx >= MAX_PLANES) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
                               "plane index %" PRIu32 " greater than max of %d", plane_idx,
                               MAX_PLANES);
        return;
    }

    uint8_t plane_bitmask = 1 << plane_idx;
    if (buffer_params->plane_bitmask & plane_bitmask) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
                               "plane %" PRIu32 " already set", plane_idx);
        return;
    }
    buffer_params->plane_bitmask |= plane_bitmask;

    buffer_params->data.num_planes++;
    struct dmabuf_plane *plane = &buffer_params->data.planes[plane_idx];
    plane->fd = fd;
    plane->offset = offset;
    plane->stride = stride;
    plane->modifier = ((uint64_t)modifier_hi << 32) | (uint64_t)modifier_lo;

    zwp_linux_buffer_params_v1_add(buffer_params->remote, fd, plane_idx, offset, stride,
                                   modifier_hi, modifier_lo);
}

static void
handle_buffer_params_create(struct wl_client *client, struct wl_resource *resource, int32_t width,
                            int32_t height, uint32_t format, uint32_t flags) {
    struct server_buffer_params *buffer_params = wl_resource_get_user_data(resource);

    if (!check_buffer_params_creation(buffer_params)) {
        return;
    }
    buffer_params->created = true;

    buffer_params->data.width = width;
    buffer_params->data.height = height;
    buffer_params->data.format = format;
    buffer_params->data.flags = flags;

    zwp_linux_buffer_params_v1_add_listener(buffer_params->remote, &buffer_params_listener,
                                            buffer_params);
    zwp_linux_buffer_params_v1_create(buffer_params->remote, width, height, format, flags);

    wl_display_roundtrip(buffer_params->parent->remote_display);
}

static void
handle_buffer_params_create_immed(struct wl_client *client, struct wl_resource *resource,
                                  uint32_t id, int32_t width, int32_t height, uint32_t format,
                                  uint32_t flags) {
    struct server_buffer_params *buffer_params = wl_resource_get_user_data(resource);

    struct wl_resource *buffer_resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
    if (!buffer_resource) {
        wl_client_post_no_memory(client);
        return;
    }

    if (!check_buffer_params_creation(buffer_params)) {
        return;
    }
    buffer_params->created = true;

    buffer_params->data.width = width;
    buffer_params->data.height = height;
    buffer_params->data.format = format;
    buffer_params->data.flags = flags;

    zwp_linux_buffer_params_v1_add_listener(buffer_params->remote, &buffer_params_listener,
                                            buffer_params);
    struct wl_buffer *remote_buffer = zwp_linux_buffer_params_v1_create_immed(
        buffer_params->remote, width, height, format, flags);

    wl_display_roundtrip(buffer_params->parent->remote_display);

    if (buffer_params->failed) {
        for (uint8_t i = 0; i < buffer_params->data.num_planes; i++) {
            close(buffer_params->data.planes[i].fd);
        }
        wl_buffer_destroy(remote_buffer);

        server_buffer_create_invalid(buffer_resource);

        return;
    }

    create_buffer(buffer_params, remote_buffer, buffer_resource);
}

static void
buffer_params_destroy(struct wl_resource *resource) {
    struct server_buffer_params *buffer_params = wl_resource_get_user_data(resource);

    if (!buffer_params->created) {
        for (uint8_t i = 0; i < MAX_PLANES; i++) {
            if (buffer_params->plane_bitmask & (1 << i)) {
                close(buffer_params->data.planes[i].fd);
            }
        }
    }

    zwp_linux_buffer_params_v1_destroy(buffer_params->remote);
    free(buffer_params);
}

static const struct zwp_linux_buffer_params_v1_interface buffer_params_impl = {
    .destroy = handle_buffer_params_destroy,
    .add = handle_buffer_params_add,
    .create = handle_buffer_params_create,
    .create_immed = handle_buffer_params_create_immed,
};

static void
handle_dmabuf_feedback_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
dmabuf_feedback_destroy(struct wl_resource *resource) {
    struct server_dmabuf_feedback *dmabuf_feedback = wl_resource_get_user_data(resource);

    zwp_linux_dmabuf_feedback_v1_destroy(dmabuf_feedback->remote);
    free(dmabuf_feedback);
}

static const struct zwp_linux_dmabuf_feedback_v1_interface dmabuf_feedback_impl = {
    .destroy = handle_dmabuf_feedback_destroy,
};

static void
handle_linux_dmabuf_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_linux_dmabuf_create_params(struct wl_client *client, struct wl_resource *resource,
                                  uint32_t id) {
    struct server_linux_dmabuf *linux_dmabuf = server_linux_dmabuf_from_resource(resource);

    struct wl_resource *buffer_params_resource = wl_resource_create(
        client, &zwp_linux_buffer_params_v1_interface, wl_resource_get_version(resource), id);
    if (!buffer_params_resource) {
        wl_client_post_no_memory(client);
        return;
    }

    struct server_buffer_params *buffer_params = calloc(1, sizeof(*buffer_params));
    if (!buffer_params) {
        wl_client_post_no_memory(client);
        return;
    }

    buffer_params->parent = linux_dmabuf;
    buffer_params->remote = zwp_linux_dmabuf_v1_create_params(linux_dmabuf->remote);

    wl_resource_set_implementation(buffer_params_resource, &buffer_params_impl, buffer_params,
                                   buffer_params_destroy);

    buffer_params->resource = buffer_params_resource;
}

static void
handle_linux_dmabuf_get_default_feedback(struct wl_client *client, struct wl_resource *resource,
                                         uint32_t id) {
    struct server_linux_dmabuf *linux_dmabuf = server_linux_dmabuf_from_resource(resource);

    struct wl_resource *dmabuf_feedback_resource = wl_resource_create(
        client, &zwp_linux_dmabuf_feedback_v1_interface, wl_resource_get_version(resource), id);
    if (!dmabuf_feedback_resource) {
        wl_client_post_no_memory(client);
        return;
    }

    struct server_dmabuf_feedback *dmabuf_feedback = calloc(1, sizeof(*dmabuf_feedback));
    if (!dmabuf_feedback) {
        wl_client_post_no_memory(client);
        return;
    }

    dmabuf_feedback->remote = zwp_linux_dmabuf_v1_get_default_feedback(linux_dmabuf->remote);

    wl_resource_set_implementation(dmabuf_feedback_resource, &dmabuf_feedback_impl, dmabuf_feedback,
                                   dmabuf_feedback_destroy);

    dmabuf_feedback->resource = dmabuf_feedback_resource;

    zwp_linux_dmabuf_feedback_v1_add_listener(dmabuf_feedback->remote, &dmabuf_feedback_listener,
                                              dmabuf_feedback);

    wl_display_roundtrip(linux_dmabuf->remote_display);
}

static void
handle_linux_dmabuf_get_surface_feedback(struct wl_client *client, struct wl_resource *resource,
                                         uint32_t id, struct wl_resource *surface_resource) {
    struct server_linux_dmabuf *linux_dmabuf = server_linux_dmabuf_from_resource(resource);
    struct server_surface *surface = server_surface_from_resource(surface_resource);

    struct wl_resource *dmabuf_feedback_resource = wl_resource_create(
        client, &zwp_linux_dmabuf_feedback_v1_interface, wl_resource_get_version(resource), id);
    if (!dmabuf_feedback_resource) {
        wl_client_post_no_memory(client);
        return;
    }

    struct server_dmabuf_feedback *dmabuf_feedback = calloc(1, sizeof(*dmabuf_feedback));
    if (!dmabuf_feedback) {
        wl_client_post_no_memory(client);
        return;
    }

    dmabuf_feedback->remote =
        zwp_linux_dmabuf_v1_get_surface_feedback(linux_dmabuf->remote, surface->remote);

    wl_resource_set_implementation(dmabuf_feedback_resource, &dmabuf_feedback_impl, dmabuf_feedback,
                                   dmabuf_feedback_destroy);

    dmabuf_feedback->resource = dmabuf_feedback_resource;

    zwp_linux_dmabuf_feedback_v1_add_listener(dmabuf_feedback->remote, &dmabuf_feedback_listener,
                                              dmabuf_feedback);

    wl_display_roundtrip(linux_dmabuf->remote_display);
}

static void
linux_dmabuf_destroy(struct wl_resource *resource) {
    // Unused.
}

static const struct zwp_linux_dmabuf_v1_interface linux_dmabuf_impl = {
    .destroy = handle_linux_dmabuf_destroy,
    .create_params = handle_linux_dmabuf_create_params,
    .get_default_feedback = handle_linux_dmabuf_get_default_feedback,
    .get_surface_feedback = handle_linux_dmabuf_get_surface_feedback,
};

struct server_linux_dmabuf *
server_linux_dmabuf_from_resource(struct wl_resource *resource) {
    ww_assert(
        wl_resource_instance_of(resource, &zwp_linux_dmabuf_v1_interface, &linux_dmabuf_impl));
    return wl_resource_get_user_data(resource);
}

static void
handle_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= VERSION);
    struct server_linux_dmabuf *linux_dmabuf = data;

    struct wl_resource *resource =
        wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }

    if (version < 4) {
        // Supporting versions of the protocol older than 4 would require us to send format and
        // modifier events whenever the global is bound. It's doable without being the one talking
        // to DRM, but it's annoying and anyone on a remotely up-to-date system should be on V4 by
        // now.
        wl_client_post_implementation_error(client,
                                            "zwp_linux_dmabuf_v1 version < 4 is unsupported");
        return;
    }

    wl_resource_set_implementation(resource, &linux_dmabuf_impl, linux_dmabuf,
                                   linux_dmabuf_destroy);
}

struct server_linux_dmabuf *
server_linux_dmabuf_create(struct server *server, struct zwp_linux_dmabuf_v1 *remote) {
    struct server_linux_dmabuf *linux_dmabuf = calloc(1, sizeof(*linux_dmabuf));
    if (!linux_dmabuf) {
        LOG(LOG_ERROR, "failed to allocate server_linux_dmabuf");
        return NULL;
    }

    linux_dmabuf->remote_display = server->remote.display;
    linux_dmabuf->remote = remote;

    linux_dmabuf->global = wl_global_create(server->display, &zwp_linux_dmabuf_v1_interface,
                                            VERSION, linux_dmabuf, handle_bind);

    linux_dmabuf->display_destroy.notify = on_display_destroy;

    wl_display_add_destroy_listener(server->display, &linux_dmabuf->display_destroy);

    return linux_dmabuf;
}
