#ifndef WAYWALL_COMPOSITOR_H
#define WAYWALL_COMPOSITOR_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct compositor {
    // Server
    struct wl_display *display;
    const char *socket_name;

    struct wl_event_source *src_remote, *src_sigint;

    // Server globals
    struct {
        struct wl_global *wl_compositor;
        struct wl_list surfaces;

        struct wl_global *wl_shm;
        struct wl_global *relative_pointer;
        struct wl_global *linux_dmabuf;
        struct wl_global *xdg_decoration;
        struct wl_global *xdg_wm_base;
        struct wl_global *wl_output; // represents the remote xdg_surface
    } globals;

    // Client (remote)
    struct compositor_remote_wl {
        // Remote connection
        struct wl_display *display;
        struct wl_registry *registry;

        // Remote globals
        struct wl_compositor *compositor;
        struct wl_subcompositor *subcompositor;
        struct wl_shm *shm;
        struct wl_data_device_manager *data_device_manager;
        struct zwp_linux_dmabuf_v1 *linux_dmabuf;
        struct zwp_pointer_constraints_v1 *pointer_constraints;
        struct zwp_relative_pointer_manager_v1 *relative_pointer_manager;
        struct wp_single_pixel_buffer_manager_v1 *single_pixel_buffer_manager;
        struct wp_tearing_control_manager_v1 *tearing_control_manager;
        struct wp_viewporter *viewporter;
        struct xdg_wm_base *xdg_wm_base;

        struct wl_array shm_formats;

        struct wl_list outputs; // client_output.link
        struct wl_list seats;   // client_seat.link

        // Remote window
        struct wp_viewport *viewport;
        struct wl_buffer *buffer;
        struct wl_surface *surface;
        struct xdg_surface *xdg_surface;
        struct xdg_toplevel *xdg_toplevel;
        struct zxdg_toplevel_decoration_v1 *xdg_decoration;

        int32_t win_width, win_height;
    } remote;
};

struct compositor *compositor_create();
void compositor_destroy(struct compositor *compositor);

#endif
