#ifndef WAYWALL_COMPOSITOR_COMPOSITOR_H
#define WAYWALL_COMPOSITOR_COMPOSITOR_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

// TODO: make headless size configurable
#define HEADLESS_WIDTH 1920
#define HEADLESS_HEIGHT 1080

struct compositor_config {
    int repeat_rate, repeat_delay;
    float floating_opacity;
    float background_color[4];
    bool confine_pointer;
    const char *cursor_theme;
    int cursor_size;
    bool stop_on_close;
    const char *layout, *model, *rules, *variant, *options;
};

struct compositor *compositor_create(struct compositor_config config);

void compositor_destroy(struct compositor *compositor);

struct wl_event_loop *compositor_get_loop(struct compositor *compositor);

void compositor_load_config(struct compositor *compositor, struct compositor_config config);

bool compositor_run(struct compositor *compositor, int display_file_fd);

void compositor_stop(struct compositor *compositor);

struct compositor {
    struct comp_input *input;
    struct comp_render *render;

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
