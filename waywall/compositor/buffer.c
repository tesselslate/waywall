#include "compositor/buffer.h"
#include <stdlib.h>
#include <wayland-client.h>
#include <wayland-server.h>

void
server_buffer_destroy(struct server_buffer *buffer) {
    if (buffer->remote) {
        wl_buffer_destroy(buffer->remote);
    }
    if (buffer->type == BUFFER_DMABUF) {
        for (uint8_t i = 0; i < buffer->data.dmabuf.num_planes; i++) {
            close(buffer->data.dmabuf.planes[i].fd);
        }
    }

    free(buffer);
}

static void
on_buffer_release(void *data, struct wl_buffer *remote_buffer) {
    struct server_buffer *buffer = data;

    wl_buffer_send_release(buffer->resource);
}

const struct wl_buffer_listener server_buffer_listener = {
    .release = on_buffer_release,
};

static void
handle_server_buffer_invalid_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
server_buffer_invalid_destroy(struct wl_resource *resource) {
    struct server_buffer *buffer = wl_resource_get_user_data(resource);
    server_buffer_destroy(buffer);
}

static const struct wl_buffer_interface server_buffer_invalid_impl = {
    .destroy = handle_server_buffer_invalid_destroy,
};

void
server_buffer_make_invalid(struct wl_resource *buffer_resource) {
    struct server_buffer *buffer = calloc(1, sizeof(*buffer));

    if (!buffer) {
        struct wl_client *client = wl_resource_get_client(buffer_resource);
        wl_client_post_no_memory(client);
        return;
    }

    buffer->resource = buffer_resource;
    buffer->remote = NULL;
    buffer->type = BUFFER_INVALID;

    wl_resource_set_implementation(buffer_resource, &server_buffer_invalid_impl, buffer,
                                   server_buffer_invalid_destroy);
}
