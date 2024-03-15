#ifndef WAYWALL_SERVER_WP_LINUX_DMABUF_H
#define WAYWALL_SERVER_WP_LINUX_DMABUF_H

#include <wayland-server-core.h>

struct server;

struct server_linux_dmabuf_g {
    struct wl_global *global;

    struct zwp_linux_dmabuf_v1 *remote;
    struct wl_display *remote_display;

    struct wl_listener on_display_destroy;
};

struct server_linux_buffer_params {
    struct wl_resource *resource;

    struct zwp_linux_buffer_params_v1 *remote;
    struct wl_display *remote_display;

    struct dmabuf_buffer_data *data;
    bool used; // whether or not a create/create_immed request has been issued
    bool ok;   // whether or not the buffer creation succeeded

    struct server_buffer *buffer;
};

struct server_linux_dmabuf_feedback {
    struct wl_resource *resource;

    struct zwp_linux_dmabuf_feedback_v1 *remote;
};

struct server_linux_dmabuf_g *server_linux_dmabuf_g_create(struct server *server);

#endif
