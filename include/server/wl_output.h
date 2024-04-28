#ifndef WAYWALL_SERVER_WL_OUTPUT_H
#define WAYWALL_SERVER_WL_OUTPUT_H

#include "server/server.h"
#include <wayland-server-core.h>
#include <wayland-util.h>

struct server_output {
    struct wl_global *global;
    struct wl_list objects; // wl_resource link

    struct server_ui *ui;

    struct wl_listener on_resize;
    struct wl_listener on_display_destroy;
};

struct server_output *server_output_create(struct server *server, struct server_ui *ui);

#endif
