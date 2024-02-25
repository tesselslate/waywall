#ifndef WAYWALL_SERVER_BUFFER_H
#define WAYWALL_SERVER_BUFFER_H

#include <wayland-client.h>
#include <wayland-server.h>

struct server_buffer {
    struct wl_resource *resource;
    struct wl_buffer *remote;

    enum server_buffer_type {
        SERVER_BUFFER_SHM,
        SERVER_BUFFER_DMABUF,
        SERVER_BUFFER_INVALID,
    } type;
    union {
        struct server_shm_buffer_data *shm;
        // TODO: dmabuf data
    } data;
};

extern const struct wl_buffer_listener server_buffer_listener;
extern const struct wl_buffer_interface server_buffer_impl;

int server_buffer_create_invalid(struct wl_resource *resource);
void server_buffer_free(struct server_buffer *buffer);
struct server_buffer *server_buffer_from_resource(struct wl_resource *resource);

#endif
