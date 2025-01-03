#include "server/wl_shm.h"
#include "server/backend.h"
#include "server/buffer.h"
#include "server/server.h"
#include "util/alloc.h"
#include "util/prelude.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <wayland-server-protocol.h>

#define SRV_SHM_VERSION 1

struct shm_buffer_data {
    int32_t width, height;
};

static void
shm_buffer_destroy(void *data) {
    struct shm_buffer_data *buffer_data = data;

    free(buffer_data);
}

static void
shm_buffer_size(void *data, int32_t *width, int32_t *height) {
    struct shm_buffer_data *buffer_data = data;

    *width = buffer_data->width;
    *height = buffer_data->height;
}

static const struct server_buffer_impl shm_buffer_impl = {
    .name = SERVER_BUFFER_SHM,

    .destroy = shm_buffer_destroy,
    .size = shm_buffer_size,
};

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

    if (offset + (height * stride) > shm_pool->sz) {
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

    struct shm_buffer_data *buffer_data = zalloc(1, sizeof(*buffer_data));

    buffer_data->width = width;
    buffer_data->height = height;

    struct wl_resource *buffer_resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
    check_alloc(buffer_resource);

    struct wl_buffer *remote =
        wl_shm_pool_create_buffer(shm_pool->remote, offset, width, height, stride, format);
    check_alloc(remote);

    server_buffer_create(buffer_resource, remote, &shm_buffer_impl, buffer_data);

    return;
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
    // Unused.
}

static void
shm_create_pool(struct wl_client *client, struct wl_resource *resource, uint32_t id, int32_t fd,
                int32_t size) {
    struct server_shm *shm = wl_resource_get_user_data(resource);

    struct server_shm_pool *shm_pool = zalloc(1, sizeof(*shm_pool));

    shm_pool->resource =
        wl_resource_create(client, &wl_shm_pool_interface, wl_resource_get_version(resource), id);
    check_alloc(shm_pool->resource);
    wl_resource_set_implementation(shm_pool->resource, &shm_pool_impl, shm_pool,
                                   shm_pool_resource_destroy);

    shm_pool->formats = shm->formats;
    shm_pool->fd = fd;
    shm_pool->sz = size;

    shm_pool->remote = wl_shm_create_pool(shm->remote, fd, size);
    check_alloc(shm_pool->remote);
}

static const struct wl_shm_interface shm_impl = {
    .create_pool = shm_create_pool,
};

static void
on_global_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SRV_SHM_VERSION);

    struct server_shm *shm = data;

    struct wl_resource *resource = wl_resource_create(client, &wl_shm_interface, version, id);
    check_alloc(resource);
    wl_resource_set_implementation(resource, &shm_impl, shm, shm_resource_destroy);

    wl_list_insert(&shm->objects, wl_resource_get_link(resource));
}

static void
on_shm_format(struct wl_listener *listener, void *data) {
    struct server_shm *shm = wl_container_of(listener, shm, on_shm_format);
    uint32_t *format = data;

    struct wl_resource *resource;
    wl_resource_for_each(resource, &shm->objects) {
        wl_shm_send_format(resource, *format);
    }
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_shm *shm = wl_container_of(listener, shm, on_display_destroy);

    wl_global_destroy(shm->global);

    wl_list_remove(&shm->on_shm_format.link);
    wl_list_remove(&shm->on_display_destroy.link);

    free(shm);
}

struct server_shm *
server_shm_create(struct server *server) {
    struct server_shm *shm = zalloc(1, sizeof(*shm));

    shm->global =
        wl_global_create(server->display, &wl_shm_interface, SRV_SHM_VERSION, shm, on_global_bind);
    check_alloc(shm->global);

    wl_list_init(&shm->objects);
    shm->remote = server->backend->shm;
    shm->formats = &server->backend->shm_formats;

    shm->on_shm_format.notify = on_shm_format;
    wl_signal_add(&server->backend->events.shm_format, &shm->on_shm_format);

    shm->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &shm->on_display_destroy);

    return shm;
}
