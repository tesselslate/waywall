#ifndef WAYWALL_SERVER_BUFFER_H
#define WAYWALL_SERVER_BUFFER_H

#include <wayland-client.h>
#include <wayland-server.h>

struct server_buffer {
    struct wl_resource *resource;
    struct wl_buffer *remote;

    const struct server_buffer_impl *impl;
    void *data;
};

struct server_buffer_impl {
    const char *name;

    void (*destroy)(void *data);
    void (*size)(void *data, uint32_t *width, uint32_t *height);
};

struct server_buffer *server_buffer_create(struct wl_resource *resource, struct wl_buffer *remote,
                                           const struct server_buffer_impl *impl, void *data);
struct server_buffer *server_buffer_create_invalid(struct wl_resource *resource);
struct server_buffer *server_buffer_from_resource(struct wl_resource *resource);
void server_buffer_get_size(struct server_buffer *buffer, uint32_t *width, uint32_t *height);
void server_buffer_validate(struct server_buffer *buffer, struct wl_buffer *remote,
                            const struct server_buffer_impl *impl, void *data);

#endif
