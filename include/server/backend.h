#ifndef WAYWALL_SERVER_BACKEND_H
#define WAYWALL_SERVER_BACKEND_H

#include <stdint.h>
#include <wayland-server-core.h>

struct server_backend {
    struct wl_display *display;
    struct wl_registry *registry;

    struct {
        struct wl_list names; // seat_name.link

        struct wl_seat *remote;
        uint32_t caps;

        struct wl_keyboard *keyboard;
        struct wl_pointer *pointer;
    } seat;
    struct wl_array shm_formats; // data: uint32_t

    struct wl_compositor *compositor;
    struct zwp_linux_dmabuf_v1 *linux_dmabuf;
    struct zwp_pointer_constraints_v1 *pointer_constraints;
    struct zwp_relative_pointer_manager_v1 *relative_pointer_manager;
    struct wl_shm *shm;
    struct wl_subcompositor *subcompositor;
    struct wp_viewporter *viewporter;
    struct xdg_wm_base *xdg_wm_base;

    struct {
        struct wl_signal seat_keyboard; // data: NULL
        struct wl_signal seat_pointer;  // data: NULL
        struct wl_signal shm_format;    // data: uint32_t *
    } events;
};

struct server_backend *server_backend_create();
void server_backend_destroy(struct server_backend *backend);

#endif
