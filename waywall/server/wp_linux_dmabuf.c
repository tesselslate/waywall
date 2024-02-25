#include "server/wp_linux_dmabuf.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "linux-dmabuf-v1-server-protocol.h"
#include "server/buffer.h"
#include "server/server.h"
#include "server/wl_compositor.h"
#include "util.h"
#include <unistd.h>

/*
 * TODO: Investigate the usage of wl_event_queue.
 * It would be nice to not have to do a full remote display roundtrip whenever certain requests are
 * issued.
 */

#define SRV_LINUX_DMABUF_VERSION 4

static void
buffer_resource_destroy(struct wl_resource *resource) {
    struct server_buffer *buffer = server_buffer_from_resource(resource);
    ww_assert(buffer->type == SERVER_BUFFER_DMABUF);

    for (size_t i = 0; i < STATIC_ARRLEN(buffer->data.dmabuf->planes); i++) {
        uint32_t mask = (1 << i);
        if (buffer->data.dmabuf->planes_set & mask) {
            close(buffer->data.dmabuf->planes[i].fd);
        }
    }

    free(buffer->data.dmabuf);
    server_buffer_free(buffer);
}

static void
validate_buffer(struct server_linux_buffer_params *buffer_params, struct server_buffer *buffer) {
    buffer->type = SERVER_BUFFER_DMABUF;
    buffer->data.dmabuf = buffer_params->data;

    // Update the resource destroy function now that we have a valid buffer.
    wl_resource_set_implementation(buffer->resource, &server_buffer_impl, buffer,
                                   buffer_resource_destroy);

    wl_buffer_add_listener(buffer->remote, &server_buffer_listener, buffer);
}

static void
on_linux_buffer_params_created(void *data, struct zwp_linux_buffer_params_v1 *wl,
                               struct wl_buffer *buffer) {
    struct server_linux_buffer_params *buffer_params = data;

    buffer_params->buffer->remote = buffer;
    validate_buffer(buffer_params, buffer_params->buffer);
    buffer_params->ok = true;
}

static void
on_linux_buffer_params_failed(void *data, struct zwp_linux_buffer_params_v1 *wl) {
    struct server_linux_buffer_params *buffer_params = data;

    buffer_params->ok = false;
}

static const struct zwp_linux_buffer_params_v1_listener linux_buffer_params_listener = {
    .created = on_linux_buffer_params_created,
    .failed = on_linux_buffer_params_failed,
};

static void
on_linux_dmabuf_feedback_done(void *data, struct zwp_linux_dmabuf_feedback_v1 *wl) {
    struct server_linux_dmabuf_feedback *feedback = data;

    zwp_linux_dmabuf_feedback_v1_send_done(feedback->resource);
}

static void
on_linux_dmabuf_feedback_format_table(void *data, struct zwp_linux_dmabuf_feedback_v1 *wl,
                                      int32_t fd, uint32_t size) {
    struct server_linux_dmabuf_feedback *feedback = data;

    zwp_linux_dmabuf_feedback_v1_send_format_table(feedback->resource, fd, size);
    close(fd);
}

static void
on_linux_dmabuf_feedback_main_device(void *data, struct zwp_linux_dmabuf_feedback_v1 *wl,
                                     struct wl_array *device) {
    struct server_linux_dmabuf_feedback *feedback = data;

    zwp_linux_dmabuf_feedback_v1_send_main_device(feedback->resource, device);
}

static void
on_linux_dmabuf_feedback_tranche_done(void *data, struct zwp_linux_dmabuf_feedback_v1 *wl) {
    struct server_linux_dmabuf_feedback *feedback = data;

    zwp_linux_dmabuf_feedback_v1_send_tranche_done(feedback->resource);
}

static void
on_linux_dmabuf_feedback_tranche_flags(void *data, struct zwp_linux_dmabuf_feedback_v1 *wl,
                                       uint32_t flags) {
    struct server_linux_dmabuf_feedback *feedback = data;

    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(feedback->resource, flags);
}

static void
on_linux_dmabuf_feedback_tranche_formats(void *data, struct zwp_linux_dmabuf_feedback_v1 *wl,
                                         struct wl_array *indices) {
    struct server_linux_dmabuf_feedback *feedback = data;

    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(feedback->resource, indices);
}

static void
on_linux_dmabuf_feedback_tranche_target_device(void *data, struct zwp_linux_dmabuf_feedback_v1 *wl,
                                               struct wl_array *device) {
    struct server_linux_dmabuf_feedback *feedback = data;

    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(feedback->resource, device);
}

static const struct zwp_linux_dmabuf_feedback_v1_listener linux_dmabuf_feedback_listener = {
    .done = on_linux_dmabuf_feedback_done,
    .format_table = on_linux_dmabuf_feedback_format_table,
    .main_device = on_linux_dmabuf_feedback_main_device,
    .tranche_done = on_linux_dmabuf_feedback_tranche_done,
    .tranche_flags = on_linux_dmabuf_feedback_tranche_flags,
    .tranche_formats = on_linux_dmabuf_feedback_tranche_formats,
    .tranche_target_device = on_linux_dmabuf_feedback_tranche_target_device,
};

static void
linux_buffer_params_resource_destroy(struct wl_resource *resource) {
    struct server_linux_buffer_params *buffer_params = wl_resource_get_user_data(resource);

    if (!buffer_params->ok) {
        for (size_t i = 0; i < STATIC_ARRLEN(buffer_params->data->planes); i++) {
            uint32_t mask = (1 << i);
            if (mask & buffer_params->data->planes_set) {
                close(buffer_params->data->planes[i].fd);
            }
        }
        free(buffer_params->data);
    }
    zwp_linux_buffer_params_v1_destroy(buffer_params->remote);
    free(buffer_params);
}

static void
linux_buffer_params_add(struct wl_client *client, struct wl_resource *resource, int32_t fd,
                        uint32_t plane_idx, uint32_t offset, uint32_t stride, uint32_t modifier_hi,
                        uint32_t modifier_lo) {
    struct server_linux_buffer_params *buffer_params = wl_resource_get_user_data(resource);

    if (plane_idx >= STATIC_ARRLEN(buffer_params->data->planes)) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
                               "plane %" PRIu32 " exceeds max of 4", plane_idx);
        return;
    }

    uint32_t mask = (1 << plane_idx);
    if (mask & buffer_params->data->planes_set) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
                               "plane %" PRIu32 " already set", plane_idx);
        return;
    }

    uint64_t modifier = ((uint64_t)modifier_hi << 32) + (uint64_t)modifier_lo;
    buffer_params->data->planes[plane_idx].fd = fd;
    buffer_params->data->planes[plane_idx].offset = offset;
    buffer_params->data->planes[plane_idx].stride = stride;
    buffer_params->data->planes[plane_idx].modifier = modifier;
    buffer_params->data->planes_set |= mask;

    zwp_linux_buffer_params_v1_add(buffer_params->remote, fd, plane_idx, offset, stride,
                                   modifier_hi, modifier_lo);
}

static void
linux_buffer_params_create(struct wl_client *client, struct wl_resource *resource, int32_t width,
                           int32_t height, uint32_t format, uint32_t flags) {
    struct server_linux_buffer_params *buffer_params = wl_resource_get_user_data(resource);

    if (buffer_params->used) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                               "cannot call create on the same zwp_linux_buffer_params twice");
        return;
    }

    struct wl_resource *buffer_resource = wl_resource_create(client, &wl_buffer_interface, 1, 0);
    if (!buffer_resource) {
        wl_resource_post_no_memory(resource);
        return;
    }
    if (server_buffer_create_invalid(buffer_resource) == 0) {
        ww_log(LOG_ERROR, "failed to create invalid server_buffer");
        return;
    }
    buffer_params->buffer = server_buffer_from_resource(buffer_resource);

    buffer_params->data->width = width;
    buffer_params->data->height = height;
    buffer_params->data->format = format;
    buffer_params->data->flags = flags;

    // There are a lot of ways this request can fail, many of which are too annoying to check for.
    // Mesa should get it right anyways.
    buffer_params->used = true;
    zwp_linux_buffer_params_v1_create(buffer_params->remote, width, height, format, flags);
    wl_display_roundtrip(buffer_params->remote_display);

    if (buffer_params->ok) {
        zwp_linux_buffer_params_v1_send_created(buffer_params->resource,
                                                buffer_params->buffer->resource);
    } else {
        zwp_linux_buffer_params_v1_send_failed(buffer_params->resource);
    }
}

static void
linux_buffer_params_create_immed(struct wl_client *client, struct wl_resource *resource,
                                 uint32_t id, int32_t width, int32_t height, uint32_t format,
                                 uint32_t flags) {
    struct server_linux_buffer_params *buffer_params = wl_resource_get_user_data(resource);

    if (buffer_params->used) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                               "cannot call create on the same zwp_linux_buffer_params twice");
        return;
    }

    struct wl_resource *buffer_resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
    if (!buffer_resource) {
        wl_resource_post_no_memory(resource);
        return;
    }
    if (server_buffer_create_invalid(buffer_resource) == 0) {
        ww_log(LOG_ERROR, "failed to create invalid server_buffer");
        return;
    }
    buffer_params->buffer = server_buffer_from_resource(buffer_resource);

    buffer_params->data->width = width;
    buffer_params->data->height = height;
    buffer_params->data->format = format;
    buffer_params->data->flags = flags;

    // There are a lot of ways this request can fail, many of which are too annoying to check for.
    // Mesa should get it right anyways.
    buffer_params->used = true;
    buffer_params->ok = true;
    buffer_params->buffer->remote = zwp_linux_buffer_params_v1_create_immed(
        buffer_params->remote, width, height, format, flags);
    wl_display_roundtrip(buffer_params->remote_display);

    if (buffer_params->ok) {
        validate_buffer(buffer_params, buffer_params->buffer);
    } else {
        zwp_linux_buffer_params_v1_send_failed(buffer_params->resource);
    }
}

static void
linux_buffer_params_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct zwp_linux_buffer_params_v1_interface linux_buffer_params_impl = {
    .add = linux_buffer_params_add,
    .create = linux_buffer_params_create,
    .create_immed = linux_buffer_params_create_immed,
    .destroy = linux_buffer_params_destroy,
};

static void
linux_dmabuf_feedback_resource_destroy(struct wl_resource *resource) {
    struct server_linux_dmabuf_feedback *feedback = wl_resource_get_user_data(resource);

    zwp_linux_dmabuf_feedback_v1_destroy(feedback->remote);
    free(feedback);
}

static void
linux_dmabuf_feedback_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct zwp_linux_dmabuf_feedback_v1_interface linux_dmabuf_feedback_impl = {
    .destroy = linux_dmabuf_feedback_destroy,
};

static void
linux_dmabuf_resource_destroy(struct wl_resource *resource) {
    struct server_linux_dmabuf *linux_dmabuf = wl_resource_get_user_data(resource);

    free(linux_dmabuf);
}

static void
linux_dmabuf_create_params(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_linux_dmabuf *linux_dmabuf = wl_resource_get_user_data(resource);

    struct server_dmabuf_buffer_data *dmabuf_data = calloc(1, sizeof(*dmabuf_data));
    if (!dmabuf_data) {
        ww_log(LOG_WARN, "failed to allocate server_dmabuf_buffer_data");
        wl_resource_post_no_memory(resource);
        return;
    }

    struct server_linux_buffer_params *buffer_params = calloc(1, sizeof(*buffer_params));
    if (!buffer_params) {
        ww_log(LOG_WARN, "failed to allocate server_linux_buffer_params");
        free(dmabuf_data);
        wl_resource_post_no_memory(resource);
        return;
    }

    buffer_params->resource = wl_resource_create(client, &zwp_linux_buffer_params_v1_interface,
                                                 wl_resource_get_version(resource), id);
    if (!buffer_params->resource) {
        free(dmabuf_data);
        free(buffer_params);
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(buffer_params->resource, &linux_buffer_params_impl,
                                   buffer_params, linux_buffer_params_resource_destroy);
    wl_resource_set_user_data(buffer_params->resource, buffer_params);

    buffer_params->remote = zwp_linux_dmabuf_v1_create_params(linux_dmabuf->remote);
    ww_assert(buffer_params->remote);
    zwp_linux_buffer_params_v1_add_listener(buffer_params->remote, &linux_buffer_params_listener,
                                            buffer_params);

    buffer_params->remote_display = linux_dmabuf->remote_display;
}

static void
linux_dmabuf_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
linux_dmabuf_get_default_feedback(struct wl_client *client, struct wl_resource *resource,
                                  uint32_t id) {
    struct server_linux_dmabuf *linux_dmabuf = wl_resource_get_user_data(resource);

    struct server_linux_dmabuf_feedback *feedback = calloc(1, sizeof(*feedback));
    if (!feedback) {
        ww_log(LOG_WARN, "failed to allocate server_linux_dmabuf_feedback");
        wl_resource_post_no_memory(resource);
        return;
    }

    feedback->resource = wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface,
                                            wl_resource_get_version(resource), id);
    if (!feedback->resource) {
        free(feedback);
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(feedback->resource, &linux_dmabuf_feedback_impl, feedback,
                                   linux_dmabuf_feedback_resource_destroy);
    wl_resource_set_user_data(feedback->resource, feedback);

    feedback->remote = zwp_linux_dmabuf_v1_get_default_feedback(linux_dmabuf->remote);
    ww_assert(feedback->remote);
    zwp_linux_dmabuf_feedback_v1_add_listener(feedback->remote, &linux_dmabuf_feedback_listener,
                                              feedback);
}

static void
linux_dmabuf_get_surface_feedback(struct wl_client *client, struct wl_resource *resource,
                                  uint32_t id, struct wl_resource *surface_resource) {
    struct server_linux_dmabuf *linux_dmabuf = wl_resource_get_user_data(resource);
    struct server_surface *surface = server_surface_from_resource(surface_resource);

    struct server_linux_dmabuf_feedback *feedback = calloc(1, sizeof(*feedback));
    if (!feedback) {
        ww_log(LOG_WARN, "failed to allocate server_linux_dmabuf_feedback");
        wl_resource_post_no_memory(resource);
        return;
    }

    feedback->resource = wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface,
                                            wl_resource_get_version(resource), id);
    if (!feedback->resource) {
        free(feedback);
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(feedback->resource, &linux_dmabuf_feedback_impl, feedback,
                                   linux_dmabuf_feedback_resource_destroy);
    wl_resource_set_user_data(feedback->resource, feedback);

    feedback->remote =
        zwp_linux_dmabuf_v1_get_surface_feedback(linux_dmabuf->remote, surface->remote);
    ww_assert(feedback->remote);
    zwp_linux_dmabuf_feedback_v1_add_listener(feedback->remote, &linux_dmabuf_feedback_listener,
                                              feedback);
}

static const struct zwp_linux_dmabuf_v1_interface linux_dmabuf_impl = {
    .create_params = linux_dmabuf_create_params,
    .destroy = linux_dmabuf_destroy,
    .get_default_feedback = linux_dmabuf_get_default_feedback,
    .get_surface_feedback = linux_dmabuf_get_surface_feedback,
};

static void
on_global_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SRV_LINUX_DMABUF_VERSION);

    if (version <= ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION) {
        // Supporting versions older than v4 would require us to send the `format` and `modifier`
        // events which would become a bit of a hassle.
        wl_client_post_implementation_error(client,
                                            "zwp_linux_dmabuf versions below 4 are unsupported");
        return;
    }

    struct server_linux_dmabuf_g *linux_dmabuf_g = data;

    struct server_linux_dmabuf *linux_dmabuf = calloc(1, sizeof(*linux_dmabuf));
    if (!linux_dmabuf) {
        ww_log(LOG_WARN, "failed to allocate server_linux_dmabuf");
        wl_client_post_no_memory(client);
        return;
    }

    linux_dmabuf->resource =
        wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, version, id);
    if (!linux_dmabuf->resource) {
        free(linux_dmabuf);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(linux_dmabuf->resource, &linux_dmabuf_impl, linux_dmabuf,
                                   linux_dmabuf_resource_destroy);
    wl_resource_set_user_data(linux_dmabuf->resource, linux_dmabuf);

    linux_dmabuf->remote = linux_dmabuf_g->remote;
    linux_dmabuf->remote_display = linux_dmabuf_g->remote_display;
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_linux_dmabuf_g *linux_dmabuf_g =
        wl_container_of(listener, linux_dmabuf_g, on_display_destroy);

    wl_global_destroy(linux_dmabuf_g->global);

    wl_list_remove(&linux_dmabuf_g->on_display_destroy.link);

    free(linux_dmabuf_g);
}

struct server_linux_dmabuf_g *
server_linux_dmabuf_g_create(struct server *server) {
    struct server_linux_dmabuf_g *linux_dmabuf_g = calloc(1, sizeof(*linux_dmabuf_g));
    if (!linux_dmabuf_g) {
        ww_log(LOG_ERROR, "failed to allocate server_linux_dmabuf_g");
        return NULL;
    }

    linux_dmabuf_g->global =
        wl_global_create(server->display, &zwp_linux_dmabuf_v1_interface, SRV_LINUX_DMABUF_VERSION,
                         linux_dmabuf_g, on_global_bind);
    ww_assert(linux_dmabuf_g->global);

    linux_dmabuf_g->remote = server->backend.linux_dmabuf;
    linux_dmabuf_g->remote_display = server->backend.display;

    linux_dmabuf_g->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &linux_dmabuf_g->on_display_destroy);

    return linux_dmabuf_g;
}
