#ifndef WAYWALL_SERVER_BUFFER_H
#define WAYWALL_SERVER_BUFFER_H

#include "util/prelude.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <wayland-server-core.h>

WW_MAYBE_UNUSED static constexpr char SERVER_BUFFER_DMABUF[] = "dmabuf";
WW_MAYBE_UNUSED static constexpr char SERVER_BUFFER_SHM[] = "shm";

struct server_buffer {
    struct wl_resource *resource;
    struct wl_buffer *remote;

    const struct server_buffer_impl *impl;
    void *data;

    uint32_t refcount;
    uint32_t lockcount;

    struct {
        struct wl_signal resource_destroy; // data: struct server_buffer *
    } events;
};

struct server_buffer_impl {
    const char *name;

    void (*destroy)(void *data);
    void (*size)(void *data, int32_t *width, int32_t *height);
};

struct server_buffer *server_buffer_create(struct wl_resource *resource, struct wl_buffer *remote,
                                           const struct server_buffer_impl *impl, void *data);
struct server_buffer *server_buffer_from_resource(struct wl_resource *resource);
void server_buffer_get_size(struct server_buffer *buffer, int32_t *width, int32_t *height);
void server_buffer_lock(struct server_buffer *buffer);
struct server_buffer *server_buffer_ref(struct server_buffer *buffer);
void server_buffer_unlock(struct server_buffer *buffer);
void server_buffer_unref(struct server_buffer *buffer);

#endif
