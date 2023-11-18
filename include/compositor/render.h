#ifndef WAYWALL_COMPOSITOR_PUB_RENDER_H
#define WAYWALL_COMPOSITOR_PUB_RENDER_H

#include "compositor/compositor.h"
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>

typedef struct wlr_scene_rect render_rect_t;

enum window_layer : uint32_t {
    LAYER_UNKNOWN = 0,
    LAYER_INSTANCE = 1 << 0,
    LAYER_FLOATING = 1 << 1,
    LAYER_WALL = 1 << 2,
};

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

struct window_configure_event {
    struct window *window;
    struct wlr_box box;
};

struct window_minimize_event {
    struct window *window;
    bool minimized;
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
 *  Updates the given window's appearance to make it appear focused.
 */
void render_focus_window(struct comp_render *render, struct window *window);

/*
 *  Enables or disables the given scene layer.
 */
void render_layer_set_enabled(struct comp_render *render, enum window_layer layer, bool enabled);

/*
 *  Applies a new configuration.
 */
void render_load_config(struct comp_render *render, struct compositor_config config);

/*
 *  Returns the size of the given output.
 */
void render_output_get_size(struct output *output, int *w, int *h);

/*
 *  Recreates the Wayland output if it is destroyed.
 */
void render_recreate_output(struct comp_render *render);

/*
 *  Repositions and resizes the given rectangle.
 */
void render_rect_configure(render_rect_t *rect, struct wlr_box box);

/*
 *  Creates a new rectangle on the lock indicator layer.
 */
render_rect_t *render_rect_create(struct comp_render *render, struct wlr_box box, float color[4]);

/*
 *  Destroys the given rectangle.
 */
void render_rect_destroy(render_rect_t *rect);

/*
 *  Sets the color of the given rectangle.
 */
void render_rect_set_color(render_rect_t *rect, const float color[4]);

/*
 *  Enables or disables the given rectangle.
 */
void render_rect_set_enabled(render_rect_t *rect, bool enabled);

/*
 *  If the given coordinates overlap with any enabled window on one of the provided layers, return
 *  the topmost window with overlap. The difference between the window coordinates and provided
 *  coordinates are returned through dx and dy if they are non-NULL.
 */
struct window *render_window_at(struct comp_render *render, uint32_t layers, double x, double y,
                                double *dx, double *dy);

/*
 *  Updates the size and position of the given window.
 */
void render_window_configure(struct window *window, int x, int y, int w, int h);

/*
 *  Returns the coordinates of the window through x and y.
 */
void render_window_get_pos(struct window *window, int *x, int *y);

/*
 *  Returns the size of the window through w and h.
 */
void render_window_get_size(struct window *window, int *w, int *h);

/*
 *  Sets the destination size of the given window.
 */
void render_window_set_dest_size(struct window *window, int w, int h);

/*
 *  Enables or disables the given window. This has no effect if the window is unmapped.
 */
void render_window_set_enabled(struct window *window, bool enabled);

/*
 *  Sets the layer of the given window.
 */
void render_window_set_layer(struct window *window, enum window_layer layer);

/*
 *  Configures the given window to display at the provided coordinates.
 */
void render_window_set_pos(struct window *window, int x, int y);

#endif
