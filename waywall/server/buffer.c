#include "server/buffer.h"
#include "util/alloc.h"
#include "util/prelude.h"
#include <stdlib.h>
#include <string.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

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
server_buffer_from_resource(struct wl_resource *resource) {
    ww_assert(wl_resource_instance_of(resource, &wl_buffer_interface, &server_buffer_impl));
    return wl_resource_get_user_data(resource);
}

void
server_buffer_get_size(struct server_buffer *buffer, uint32_t *width, uint32_t *height) {
    buffer->impl->size(buffer->data, width, height);
}
