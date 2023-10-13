#ifndef WAYWALL_COMPOSITOR_P_H
#define WAYWALL_COMPOSITOR_P_H

#include "../compositor.h"
#include <wayland-server-core.h>

struct compositor {
    struct wl_display *display;
    struct wlr_allocator *allocator;
    struct wlr_backend *backend;
    struct wlr_backend *backend_wl;
    struct wlr_backend *backend_headless;
    struct wlr_compositor *compositor;
    struct wlr_renderer *renderer;
    struct wlr_export_dmabuf_manager_v1 *dmabuf_export;

    struct wlr_scene *scene;
    struct wlr_scene_tree *scene_floating;
    struct wlr_scene_tree *scene_indicators;
    struct wlr_scene_tree *scene_instances;
    struct wlr_scene_tree *scene_headless;
    struct wlr_scene_tree *scene_unknown;
    struct wlr_scene_rect *background;
    struct wlr_scene_output_layout *scene_layout;

    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_manager;
    double mouse_sens;
    struct wl_listener on_cursor_motion;
    struct wl_listener on_cursor_motion_absolute;
    struct wl_listener on_cursor_button;
    struct wl_listener on_cursor_axis;
    struct wl_listener on_cursor_frame;

    struct wlr_seat *seat;
    struct wl_list keyboards;
    struct wl_listener on_new_input;
    struct wl_listener on_request_cursor;
    struct wl_listener on_request_set_selection;

    struct wlr_output_layout *output_layout;
    struct wl_list outputs;
    struct wl_listener on_new_output;
    struct output *wl_output;
    struct output *headless_output;

    struct wlr_xwayland *xwayland;
    struct xcb_connection_t *xcb;
    struct wl_list windows;
    struct window *focused_window;
    struct window *grabbed_window;
    double grab_x, grab_y;
    bool on_wall;
    struct wl_listener on_xwayland_new_surface;
    struct wl_listener on_xwayland_ready;

    struct wl_display *remote_display;
    struct wl_pointer *remote_pointer;
    struct wl_seat *remote_seat;

    struct wlr_pointer_constraints_v1 *pointer_constraints;
    struct wlr_pointer_constraint_v1 *active_constraint;
    struct zwp_pointer_constraints_v1 *remote_pointer_constraints;
    struct zwp_locked_pointer_v1 *remote_locked_pointer;
    struct zwp_confined_pointer_v1 *remote_confined_pointer;
    struct wl_listener on_new_constraint;

    struct wlr_relative_pointer_manager_v1 *relative_pointer;
    struct zwp_relative_pointer_manager_v1 *remote_relative_pointer_manager;
    struct zwp_relative_pointer_v1 *remote_relative_pointer;

    struct compositor_config config;
    struct compositor_vtable vtable;
    bool should_stop;
};

struct keyboard {
    struct wl_list link;

    struct compositor *compositor;
    struct wlr_keyboard *wlr_keyboard;

    struct wl_listener on_modifiers;
    struct wl_listener on_key;
    struct wl_listener on_destroy;
};

struct output {
    struct wl_list link;

    struct compositor *compositor;
    struct wlr_output *wlr_output;
    struct wlr_output_layout_output *layout;
    struct wlr_scene_output *scene;

    bool headless;
    struct wl_surface *remote_surface;

    struct wl_listener on_frame;
    struct wl_listener on_request_state;
    struct wl_listener on_destroy;
};

struct pointer_constraint {
    struct compositor *compositor;
    struct wlr_pointer_constraint_v1 *constraint;

    struct wl_listener on_set_region;
    struct wl_listener on_destroy;
};

struct window {
    struct wl_list link;

    struct compositor *compositor;
    struct wlr_xwayland_surface *surface;
    struct wlr_scene_tree *scene_tree;
    struct scene_window *scene_window;
    enum compositor_wintype type;

    struct headless_view {
        struct wlr_scene_tree *tree;
        struct scene_window *scene_window;
    } headless_views[4];
    struct wlr_scene_tree *headless_tree;
    int headless_view_count;

    struct wl_listener on_associate;
    struct wl_listener on_dissociate;

    struct wl_listener on_map;
    struct wl_listener on_unmap;
    struct wl_listener on_destroy;
    struct wl_listener on_request_activate;
    struct wl_listener on_request_configure;
    struct wl_listener on_request_fullscreen;
    struct wl_listener on_request_minimize;
};

#endif
