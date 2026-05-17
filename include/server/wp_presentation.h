#pragma once

#include "config/config.h"
#include <wayland-server-core.h>
#include <wayland-util.h>

struct server;

struct server_presentation {
    struct wl_global *global;
    struct wp_presentation *remote;
    uint32_t remote_clock_id;

    struct wl_listener on_display_destroy;
};

struct server_presentation *server_presentation_create(struct server *server);
