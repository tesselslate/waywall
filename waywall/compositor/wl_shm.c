#include "compositor/wl_shm.h"
#include "compositor/buffer.h"
#include "compositor/server.h"
#include "util.h"
#include <wayland-client.h>
#include <wayland-server.h>

// TODO: formats are broken. we do not receive the list of formats because it's too late by the time
// we add our listener

/*
 *  Most functions are needed by relevant clients (GLFW, Mesa).
 *  They are all just passthrough for the most part.
 */

#define VERSION 1

struct server_shm_pool {
    struct wl_shm_pool *remote;
};

static void
on_shm_format(void *data, struct wl_shm *wl_shm, uint32_t format) {
    struct server_shm *shm = data;

    uint32_t *format_arr = wl_array_add(&shm->formats, sizeof(uint32_t));
    *format_arr = format;

    LOG(LOG_INFO, "shm format %" PRIu32, format);
}

static const struct wl_shm_listener shm_listener = {
    .format = on_shm_format,
};

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_shm *shm = wl_container_of(listener, shm, display_destroy);

    if (shm->remote) {
        wl_shm_destroy(shm->remote);
    }
    wl_global_destroy(shm->global);

    wl_array_release(&shm->formats);
    free(shm);
}

static void
server_buffer_shm_destroy(struct wl_resource *resource) {
    struct server_buffer *buffer = server_buffer_from_resource(resource);

    server_buffer_destroy(buffer);
}

static void
handle_shm_pool_create_buffer(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                              int32_t offset, int32_t width, int32_t height, int32_t stride,
                              uint32_t format) {
    struct server_shm_pool *shm_pool = wl_resource_get_user_data(resource);

    struct wl_resource *buffer_resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
    if (!buffer_resource) {
        wl_client_post_no_memory(client);
        return;
    }

    struct server_buffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) {
        wl_client_post_no_memory(client);
        return;
    }

    struct wl_buffer *remote_buffer =
        wl_shm_pool_create_buffer(shm_pool->remote, offset, width, height, stride, format);
    wl_buffer_add_listener(remote_buffer, &server_buffer_listener, buffer);

    buffer->remote = remote_buffer;
    buffer->type = BUFFER_SHM;
    buffer->data.shm.width = width;
    buffer->data.shm.height = height;

    wl_resource_set_implementation(buffer_resource, &server_buffer_impl, buffer,
                                   server_buffer_shm_destroy);

    buffer->resource = buffer_resource;
}

static void
handle_shm_pool_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_shm_pool_resize(struct wl_client *client, struct wl_resource *resource, int32_t size) {
    struct server_shm_pool *shm_pool = wl_resource_get_user_data(resource);

    wl_shm_pool_resize(shm_pool->remote, size);
}

static void
shm_pool_destroy(struct wl_resource *resource) {
    struct server_shm_pool *shm_pool = wl_resource_get_user_data(resource);

    wl_shm_pool_destroy(shm_pool->remote);
    free(shm_pool);
}

static const struct wl_shm_pool_interface shm_pool_impl = {
    .create_buffer = handle_shm_pool_create_buffer,
    .destroy = handle_shm_pool_destroy,
    .resize = handle_shm_pool_resize,
};

static void
handle_shm_create_pool(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                       int32_t fd, int32_t size) {
    struct server_shm *shm = server_shm_from_resource(resource);

    struct wl_resource *shm_pool_resource =
        wl_resource_create(client, &wl_shm_pool_interface, wl_resource_get_version(resource), id);
    if (!shm_pool_resource) {
        wl_client_post_no_memory(client);
        return;
    }

    struct server_shm_pool *shm_pool = calloc(1, sizeof(*shm_pool));
    if (!shm_pool) {
        wl_client_post_no_memory(client);
        return;
    }

    shm_pool->remote = wl_shm_create_pool(shm->remote, fd, size);
    close(fd);

    wl_resource_set_implementation(shm_pool_resource, &shm_pool_impl, shm_pool, shm_pool_destroy);
}

static void
shm_destroy(struct wl_resource *resource) {
    // Unused.
}

static const struct wl_shm_interface shm_impl = {
    .create_pool = handle_shm_create_pool,
};

struct server_shm *
server_shm_from_resource(struct wl_resource *resource) {
    ww_assert(wl_resource_instance_of(resource, &wl_shm_interface, &shm_impl));
    return wl_resource_get_user_data(resource);
}

static void
handle_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= VERSION);
    struct server_shm *shm = data;

    struct wl_resource *resource = wl_resource_create(client, &wl_shm_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }

    wl_resource_set_implementation(resource, &shm_impl, shm, shm_destroy);

    uint32_t *format;
    wl_array_for_each(format, &shm->formats) {
        wl_shm_send_format(resource, *format);
    }
}

struct server_shm *
server_shm_create(struct server *server, struct wl_shm *remote) {
    struct server_shm *shm = calloc(1, sizeof(*shm));
    if (!shm) {
        LOG(LOG_ERROR, "failed to allocate server_shm");
        return NULL;
    }

    wl_array_init(&shm->formats);

    shm->remote = remote;
    wl_shm_add_listener(shm->remote, &shm_listener, shm);

    shm->global = wl_global_create(server->display, &wl_shm_interface, VERSION, shm, handle_bind);

    shm->display_destroy.notify = on_display_destroy;

    wl_display_add_destroy_listener(server->display, &shm->display_destroy);

    return shm;
}
