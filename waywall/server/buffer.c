#include "server/buffer.h"
#include "util/alloc.h"
#include "util/prelude.h"
#include <stdlib.h>
#include <wayland-client.h>

static void
invalid_buffer_destroy(void *data) {
    // Unused.
}

static void
invalid_buffer_size(void *data, uint32_t *width, uint32_t *height) {
    ww_panic("attempted to get size of invalid buffer");
}

static const struct server_buffer_impl invalid_buffer_impl = {
    .name = "invalid",

    .destroy = invalid_buffer_destroy,
    .size = invalid_buffer_size,
};

static void
on_buffer_release(void *data, struct wl_buffer *wl) {
    struct server_buffer *buffer = data;

    wl_buffer_send_release(buffer->resource);
}

static const struct wl_buffer_listener server_buffer_listener = {
    .release = on_buffer_release,
};

static void
buffer_resource_destroy(struct wl_resource *resource) {
    struct server_buffer *buffer = wl_resource_get_user_data(resource);

    buffer->impl->destroy(buffer->data);
    if (buffer->remote) {
        wl_buffer_destroy(buffer->remote);
    }
    free(buffer);
}

static void
buffer_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct wl_buffer_interface server_buffer_impl = {
    .destroy = buffer_destroy,
};

struct server_buffer *
server_buffer_create(struct wl_resource *resource, struct wl_buffer *remote,
                     const struct server_buffer_impl *impl, void *data) {
    struct server_buffer *buffer = zalloc(1, sizeof(*buffer));

    buffer->resource = resource;
    buffer->remote = remote;
    buffer->impl = impl;
    buffer->data = data;

    wl_resource_set_implementation(resource, &server_buffer_impl, buffer, buffer_resource_destroy);
    wl_buffer_add_listener(remote, &server_buffer_listener, buffer);

    return buffer;
}

struct server_buffer *
server_buffer_create_invalid(struct wl_resource *resource) {
    struct server_buffer *buffer = zalloc(1, sizeof(*buffer));

    buffer->resource = resource;
    buffer->impl = &invalid_buffer_impl;

    wl_resource_set_implementation(resource, &server_buffer_impl, buffer, buffer_resource_destroy);

    return buffer;
}

struct server_buffer *
server_buffer_from_resource(struct wl_resource *resource) {
    ww_assert(wl_resource_instance_of(resource, &wl_buffer_interface, &server_buffer_impl));
    return wl_resource_get_user_data(resource);
}

void
server_buffer_get_size(struct server_buffer *buffer, uint32_t *width, uint32_t *height) {
    buffer->impl->size(buffer->data, width, height);
}

bool
server_buffer_is_invalid(struct server_buffer *buffer) {
    return (strcmp(buffer->impl->name, invalid_buffer_impl.name) == 0);
}

void
server_buffer_validate(struct server_buffer *buffer, struct wl_buffer *remote,
                       const struct server_buffer_impl *impl, void *data) {
    ww_assert(buffer->impl == &invalid_buffer_impl);

    buffer->remote = remote;
    buffer->impl = impl;
    buffer->data = data;

    wl_buffer_add_listener(remote, &server_buffer_listener, buffer);
}
