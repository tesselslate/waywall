#ifndef WAYWALL_SERVER_SERVER_H
#define WAYWALL_SERVER_SERVER_H

#include <wayland-client-core.h>
#include <wayland-server-core.h>

struct server_backend {
    struct wl_display *display;
    struct wl_registry *registry;
};

struct server {
    struct wl_display *display;
    struct server_backend backend;
};

struct server *server_create();
void server_destroy(struct server *server);

#endif
