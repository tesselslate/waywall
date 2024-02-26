#ifndef WAYWALL_SERVER_SERVER_H
#define WAYWALL_SERVER_SERVER_H

#include <wayland-client-core.h>
#include <wayland-server-core.h>

struct server_backend {
    struct wl_display *display;
    struct wl_registry *registry;

    struct wl_list seats;        // backend_seat.link
    struct wl_array shm_formats; // data: uint32_t

    struct wl_compositor *compositor;
    struct zwp_linux_dmabuf_v1 *linux_dmabuf;
    struct wl_shm *shm;

    struct {
        struct wl_signal shm_format; // data: uint32_t *
    } events;
};

struct server {
    struct wl_display *display;
    struct server_backend backend;

    struct server_compositor_g *compositor;
    struct server_linux_dmabuf_g *linux_dmabuf;
    struct server_shm_g *shm;
    struct server_xdg_decoration_manager_g *xdg_decoration;
    struct server_xdg_wm_base_g *xdg_shell;
};

struct server *server_create();
void server_destroy(struct server *server);

#endif
