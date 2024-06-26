#ifndef WAYWALL_SERVER_XDG_SHELL_H
#define WAYWALL_SERVER_XDG_SHELL_H

#include "util/serial.h"
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-util.h>

struct server_xdg_wm_base {
    struct wl_global *global;

    struct server *server;

    struct wl_listener on_display_destroy;
};

struct server_xdg_client {
    struct wl_resource *resource;

    struct server *server;
    struct wl_list surfaces; // server_xdg_surface.link
};

struct server_xdg_surface {
    struct wl_list link; // server_xdg_client.surfaces
    struct wl_resource *resource;

    struct server_xdg_client *xdg_wm_base;
    struct server_surface *parent;
    struct server_xdg_toplevel *child;
    struct serial_ring serials;
    bool initial_commit;
    bool initial_ack;
};

struct server_xdg_toplevel {
    struct wl_resource *resource;

    struct server_xdg_surface *parent;
    char *title;
    uint32_t width, height;

    struct server_view *view;

    struct {
        struct wl_signal destroy; // data: NULL
    } events;
};

struct server_xdg_wm_base *server_xdg_wm_base_create(struct server *server);
void server_xdg_surface_send_configure(struct server_xdg_surface *xdg_surface);
struct server_xdg_surface *server_xdg_surface_from_resource(struct wl_resource *resource);
struct server_xdg_toplevel *server_xdg_toplevel_from_resource(struct wl_resource *resource);

#endif
