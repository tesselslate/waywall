#ifndef WAYWALL_COMPOSITOR_WP_LINUX_DMABUF_H
#define WAYWALL_COMPOSITOR_WP_LINUX_DMABUF_H

#define LINUX_DMABUF_REMOTE_VERSION 4

#include "compositor/buffer.h"
#include <wayland-server-core.h>

struct server;

struct server_linux_dmabuf {
    struct zwp_linux_dmabuf_v1 *remote;
    struct wl_global *global;

    struct wl_display *remote_display;

    struct wl_listener display_destroy;
};

struct server_buffer_params {
    struct server_linux_dmabuf *parent;

    struct wl_resource *resource;
    struct zwp_linux_buffer_params_v1 *remote;

    struct buffer_data_dmabuf data;
    uint8_t plane_bitmask;

    bool failed, created;
    struct wl_resource *buffer_resource;
};

struct server_dmabuf_feedback {
    struct wl_resource *resource;
    struct zwp_linux_dmabuf_feedback_v1 *remote;
};

struct server_linux_dmabuf *server_linux_dmabuf_create(struct server *server,
                                                       struct zwp_linux_dmabuf_v1 *remote);

#endif
