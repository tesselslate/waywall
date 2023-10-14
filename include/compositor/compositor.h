#ifndef WAYWALL_COMPOSITOR_COMPOSITOR_H
#define WAYWALL_COMPOSITOR_COMPOSITOR_H

#define WAYWALL_COMPOSITOR_IMPL

#include "compositor/pub_compositor.h"
#include <wayland-server-core.h>

struct compositor {
    // Public (opaque) fields
    struct comp_input *input;
    struct comp_render *render;

    // Private fields
    struct wl_display *display;
    struct wlr_backend *backend;
    struct wlr_backend *backend_headless;
    struct wlr_backend *backend_wl;

    struct wlr_allocator *allocator;
    struct wlr_renderer *renderer;
    struct wlr_compositor *compositor;
    struct wlr_export_dmabuf_manager_v1 *dmabuf_export;

    // Remote objects
    struct {
        struct wl_display *display;
        struct wl_registry *registry;

        struct wl_pointer *pointer;
        struct wl_seat *seat;

        struct zwp_relative_pointer_manager_v1 *relative_pointer_manager;
        struct zwp_relative_pointer_v1 *relative_pointer;

        struct zwp_pointer_constraints_v1 *constraints;
    } remote;

    // Xwayland
    struct comp_xwayland *xwl;
    struct xwl_window *focused_window;
    struct wl_listener on_window_destroy;

    // State
    struct compositor_config config;
    bool should_stop;
};

#endif
