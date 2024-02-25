#include "server/wl_shm.h"
#include "server/buffer.h"
#include "server/server.h"
#include "util.h"
#include <stdlib.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server.h>

#define SRV_SHM_VERSION 1

static void
buffer_resource_destroy(struct wl_resource *resource) {
    struct server_buffer *buffer = server_buffer_from_resource(resource);
    ww_assert(buffer->type == SERVER_BUFFER_SHM);

    free(buffer->data.shm);
    server_buffer_free(buffer);
}

static void
shm_pool_resource_destroy(struct wl_resource *resource) {
    struct server_shm_pool *shm_pool = wl_resource_get_user_data(resource);

    wl_shm_pool_destroy(shm_pool->remote);
    close(shm_pool->fd);
    free(shm_pool);
}

static void
shm_pool_create_buffer(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                       int32_t offset, int32_t width, int32_t height, int32_t stride,
                       uint32_t format) {
    struct server_shm_pool *shm_pool = wl_resource_get_user_data(resource);

    if (offset + (width * stride) >= shm_pool->sz) {
        wl_resource_post_error(
            resource, WL_SHM_ERROR_INVALID_STRIDE,
            "create_buffer: invalid size: (%d + %dx%d, stride: %d) exceeds pool size (%d)",
            (int)offset, (int)width, (int)height, (int)stride, (int)shm_pool->sz);
        return;
    }

    uint32_t *fmt;
    bool ok = false;
    wl_array_for_each(fmt, shm_pool->formats) {
        if (*fmt == format) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_FORMAT,
                               "create_buffer: invalid format %" PRIu32, format);
        return;
    }

    struct server_buffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) {
        ww_log(LOG_WARN, "failed to allocate server_buffer");
        wl_client_post_no_memory(client);
        return;
    }
    struct server_shm_buffer_data *buffer_data = calloc(1, sizeof(*buffer_data));
    if (!buffer_data) {
        ww_log(LOG_WARN, "failed to allocate server_shm_buffer_data");
        free(buffer);
        wl_client_post_no_memory(client);
        return;
    }

    buffer_data->fd = dup(shm_pool->fd);
    if (buffer_data->fd == -1) {
        ww_log(LOG_WARN, "failed to dup shm_pool fd");
        free(buffer_data);
        free(buffer);
        wl_client_post_implementation_error(client, "failed to dup shm_pool fd");
        return;
    }
    buffer_data->offset = offset;
    buffer_data->width = width;
    buffer_data->height = height;
    buffer_data->stride = stride;
    buffer_data->format = format;

    buffer->resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
    ww_assert(buffer->resource);
    wl_resource_set_implementation(buffer->resource, &server_buffer_impl, buffer,
                                   buffer_resource_destroy);
    wl_resource_set_user_data(buffer->resource, buffer);

    buffer->type = SERVER_BUFFER_SHM;
    buffer->data.shm = buffer_data;

    buffer->remote =
        wl_shm_pool_create_buffer(shm_pool->remote, offset, width, height, stride, format);
    ww_assert(buffer->remote);
}

static void
shm_pool_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
shm_pool_resize(struct wl_client *client, struct wl_resource *resource, int32_t size) {
    struct server_shm_pool *shm_pool = wl_resource_get_user_data(resource);

    if (size < shm_pool->sz) {
        wl_resource_post_error(resource, WL_SHM_ERROR_INVALID_STRIDE,
                               "cannot decrease size of wl_shm_pool (fd: %d, size: %d -> %d)",
                               (int)shm_pool->fd, (int)shm_pool->sz, (int)size);
        return;
    }

    shm_pool->sz = size;
    wl_shm_pool_resize(shm_pool->remote, size);
}

static const struct wl_shm_pool_interface shm_pool_impl = {
    .create_buffer = shm_pool_create_buffer,
    .destroy = shm_pool_destroy,
    .resize = shm_pool_resize,
};

static void
shm_resource_destroy(struct wl_resource *resource) {
    struct server_shm *shm = wl_resource_get_user_data(resource);

    wl_list_remove(&shm->link);
    free(shm);
}

static void
shm_create_pool(struct wl_client *client, struct wl_resource *resource, uint32_t id, int32_t fd,
                int32_t size) {
    struct server_shm *shm = wl_resource_get_user_data(resource);

    struct server_shm_pool *shm_pool = calloc(1, sizeof(*shm_pool));
    if (!shm_pool) {
        ww_log(LOG_WARN, "failed to allocate server_shm_pool");
        wl_client_post_no_memory(client);
        return;
    }

    shm_pool->resource =
        wl_resource_create(client, &wl_shm_pool_interface, wl_resource_get_version(resource), id);
    ww_assert(shm_pool->resource);
    wl_resource_set_implementation(shm_pool->resource, &shm_pool_impl, shm_pool,
                                   shm_pool_resource_destroy);
    wl_resource_set_user_data(shm_pool->resource, shm_pool);

    shm_pool->formats = shm->formats;
    shm_pool->fd = fd;
    shm_pool->sz = size;

    shm_pool->remote = wl_shm_create_pool(shm->remote, fd, size);
    ww_assert(shm_pool->remote);
}

static const struct wl_shm_interface shm_impl = {
    .create_pool = shm_create_pool,
};

static void
on_global_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SRV_SHM_VERSION);

    struct server_shm_g *shm_g = data;

    struct server_shm *shm = calloc(1, sizeof(*shm));
    if (!shm) {
        ww_log(LOG_WARN, "failed to allocate server_shm");
        wl_client_post_no_memory(client);
        return;
    }

    shm->resource = wl_resource_create(client, &wl_shm_interface, version, id);
    ww_assert(shm->resource);
    wl_resource_set_implementation(shm->resource, &shm_impl, shm, shm_resource_destroy);
    wl_resource_set_user_data(shm->resource, shm);

    shm->formats = shm_g->formats;
    shm->remote = shm_g->remote;

    wl_list_insert(&shm_g->objects, &shm->link);
}

static void
on_shm_format(struct wl_listener *listener, void *data) {
    struct server_shm_g *shm_g = wl_container_of(listener, shm_g, on_shm_format);
    uint32_t *format = data;

    struct server_shm *shm;
    wl_list_for_each (shm, &shm_g->objects, link) {
        wl_shm_send_format(shm->resource, *format);
    }
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_shm_g *shm_g = wl_container_of(listener, shm_g, on_display_destroy);

    wl_global_destroy(shm_g->global);

    wl_list_remove(&shm_g->on_shm_format.link);
    wl_list_remove(&shm_g->on_display_destroy.link);

    free(shm_g);
}

struct server_shm_g *
server_shm_g_create(struct server *server) {
    struct server_shm_g *shm_g = calloc(1, sizeof(*shm_g));
    if (!shm_g) {
        ww_log(LOG_ERROR, "failed to allocate server_shm_g");
        return NULL;
    }

    shm_g->global = wl_global_create(server->display, &wl_shm_interface, SRV_SHM_VERSION, shm_g,
                                     on_global_bind);
    ww_assert(shm_g->global);

    wl_list_init(&shm_g->objects);
    shm_g->remote = server->backend.shm;
    shm_g->formats = &server->backend.shm_formats;

    shm_g->on_shm_format.notify = on_shm_format;
    wl_signal_add(&server->backend.events.shm_format, &shm_g->on_shm_format);

    shm_g->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &shm_g->on_display_destroy);

    return shm_g;
}
