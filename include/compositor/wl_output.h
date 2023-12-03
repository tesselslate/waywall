#ifndef WAYWALL_COMPOSITOR_WL_OUTPUT_H
#define WAYWALL_COMPOSITOR_WL_OUTPUT_H

#include <wayland-server-core.h>

struct server;

struct server_output {
    struct wl_global *global;

    struct wl_list clients; // wl_resource.link (wl_output)

    int32_t width, height, scale_factor;

    struct wl_listener display_destroy;
};

struct server_output *server_output_create(struct server *server);

#endif
