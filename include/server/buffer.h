#ifndef WAYWALL_SERVER_BUFFER_H
#define WAYWALL_SERVER_BUFFER_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define SERVER_BUFFER_DMABUF "dmabuf"
#define SERVER_BUFFER_SHM "shm"

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
struct server_buffer *server_buffer_from_resource(struct wl_resource *resource);
void server_buffer_get_size(struct server_buffer *buffer, uint32_t *width, uint32_t *height);

#endif
