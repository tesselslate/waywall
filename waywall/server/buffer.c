#include "server/buffer.h"
#include "util.h"
#include <stdlib.h>
#include <wayland-client.h>

static void
on_buffer_release(void *data, struct wl_buffer *wl) {
    struct server_buffer *buffer = data;

    wl_buffer_send_release(buffer->resource);
}

const struct wl_buffer_listener server_buffer_listener = {
    .release = on_buffer_release,
};

static void
buffer_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

const struct wl_buffer_interface server_buffer_impl = {
    .destroy = buffer_destroy,
};

static void
invalid_buffer_destroy(struct wl_resource *resource) {
    struct server_buffer *buffer = wl_resource_get_user_data(resource);
    server_buffer_free(buffer);
}

int
server_buffer_create_invalid(struct wl_resource *resource) {
    struct server_buffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) {
        struct wl_client *client = wl_resource_get_client(resource);
        wl_client_post_no_memory(client);
        return 1;
    }

    buffer->resource = resource;
    buffer->type = SERVER_BUFFER_INVALID;

    wl_resource_set_implementation(resource, &server_buffer_impl, buffer, invalid_buffer_destroy);
    wl_resource_set_user_data(buffer->resource, buffer);

    return 0;
}

void
server_buffer_free(struct server_buffer *buffer) {
    if (buffer->remote) {
        wl_buffer_destroy(buffer->remote);
    }

    switch (buffer->type) {
    case SERVER_BUFFER_SHM:
        break;
    case SERVER_BUFFER_DMABUF:
        break;
    case SERVER_BUFFER_INVALID:
        return;
    }

    free(buffer);
}

struct server_buffer *
server_buffer_from_resource(struct wl_resource *resource) {
    ww_assert(wl_resource_instance_of(resource, &wl_buffer_interface, &server_buffer_impl));
    return wl_resource_get_user_data(resource);
}
