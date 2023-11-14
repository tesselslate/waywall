#ifndef WAYWALL_COMPOSITOR_OUTPUT_H
#define WAYWALL_COMPOSITOR_OUTPUT_H

#define WAYWALL_COMPOSITOR_IMPL

#include "compositor/compositor.h"
#include "compositor/pub_render.h"
#include "compositor/scene_window.h"
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>

/*
 *  comp_render contains state related to the Wayland display (e.g. remote objects) and headless
 *  display, as well as scene information.
 */
struct comp_render {
    // Public state
    struct {
        struct wl_signal wl_output_create;  // data: comp_render.wl
        struct wl_signal wl_output_resize;  // data: comp_render.wl
        struct wl_signal wl_output_destroy; // data: comp_render.wl (partially destroyed)

        struct wl_signal window_map;       // data: window
        struct wl_signal window_unmap;     // data: window
        struct wl_signal window_configure; // data: window_configure_event (stack allocated)
        struct wl_signal window_minimize;  // data: window_minimize_event (stack allocated)
        struct wl_signal window_destroy;   // data: NULL
    } events;

    // Private state
    struct compositor *compositor;
    struct comp_xwayland *xwl;

    struct wl_listener on_xwl_window_map;
    struct wl_listener on_xwl_window_destroy;

    struct wl_list outputs; // output.link
    struct wlr_output_layout *layout;
    struct output *wl, *hl;
    struct wl_listener on_new_output;

    struct wl_list windows; // window.link

    struct wlr_scene *scene;
    struct wlr_scene_tree *tree_floating;
    struct wlr_scene_tree *tree_instance;
    struct wlr_scene_tree *tree_wall;
    struct wlr_scene_tree *tree_headless;
    struct wlr_scene_tree *tree_unknown;
    struct wlr_scene_rect *background;
    struct wlr_scene_output_layout *scene_layout;
};

/*
 *  output contains a single wlr_output and its associated state.
 */
struct output {
    struct wl_list link; // comp_render.outputs

    struct comp_render *render;
    struct wlr_output *wlr_output;
    struct wlr_output_layout_output *layout;
    struct wlr_scene_output *scene;

    bool headless;

    struct {
        struct wl_surface *surface;
        struct zwp_locked_pointer_v1 *locked_pointer;
        struct zwp_confined_pointer_v1 *confined_pointer;
    } remote;

    struct wl_listener on_frame;
    struct wl_listener on_request_state;
    struct wl_listener on_destroy;
};

/*
 *  window is used for wrapping xwl_windows and providing facilities for rendering them out on the
 *  Wayland output.
 */
struct window {
    struct wl_list link; // comp_render.windows

    struct comp_render *render;
    struct xwl_window *xwl_window;

    struct scene_window *scene_window;
    struct wlr_scene_tree *tree;

    struct wl_listener on_unmap;
    struct wl_listener on_configure;
    struct wl_listener on_minimize;
};

/*
 *  Attempts to set up render functionality for the compositor.
 */
struct comp_render *render_create(struct compositor *compositor);

/*
 *  Frees any resources allocated by the render subsystem.
 */
void render_destroy(struct comp_render *render);

/*
 *  Applies a new configuration.
 */
void render_load_config(struct comp_render *render, struct compositor_config config);

#endif
