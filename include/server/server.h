#ifndef WAYWALL_SERVER_SERVER_H
#define WAYWALL_SERVER_SERVER_H

#include "server/ui.h"
#include <wayland-client-core.h>
#include <wayland-server-core.h>

struct server_backend {
    struct wl_display *display;
    struct wl_registry *registry;

    struct wl_seat *seat;
    uint32_t seat_caps;
    struct wl_array shm_formats; // data: uint32_t

    struct wl_compositor *compositor;
    struct zwp_linux_dmabuf_v1 *linux_dmabuf;
    struct zwp_relative_pointer_manager_v1 *relative_pointer_manager;
    struct wl_shm *shm;
    struct wl_subcompositor *subcompositor;
    struct wp_viewporter *viewporter;
    struct xdg_wm_base *xdg_wm_base;

    struct {
        struct wl_signal seat_caps;  // data: uint32_t *
        struct wl_signal shm_format; // data: uint32_t *
    } events;
};

struct server {
    struct wl_display *display;
    struct server_backend backend;
    struct server_ui ui;

    struct wl_event_source *backend_source;

    struct remote_buffer_manager *remote_buf;

    struct server_compositor_g *compositor;
    struct server_linux_dmabuf_g *linux_dmabuf;
    struct server_relative_pointer_g *relative_pointer;
    struct server_seat_g *seat;
    struct server_shm_g *shm;
    struct server_xdg_decoration_manager_g *xdg_decoration;
    struct server_xdg_wm_base_g *xdg_shell;
};

struct server *server_create();
void server_destroy(struct server *server);

void server_shutdown(struct server *server);

#endif
