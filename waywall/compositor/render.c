/*
 *  The render module is responsible for managing the Wayland and headless (verification) outputs.
 *  It handles the presentation of all windows.
 *
 *  The scene has multiple subtrees (used as layers), each for a specific purpose. From top to
 *  bottom, in the following order:
 *      - Floating
 *      - Locks
 *      - Instances
 *
 *  The headless tree is used for the headless output, and the unknown tree is invisible.
 */

#define WAYWALL_COMPOSITOR_IMPL

#include "compositor/render.h"
#include "compositor/xwayland.h"
#include "util.h"
#include <stdlib.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/wayland.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

#define OUTPUT_WL_X 0
#define OUTPUT_WL_Y 0
#define OUTPUT_HL_X 16384
#define OUTPUT_HL_Y 16384

static struct window *
window_at_layer(struct comp_render *render, enum window_layer layer, double x, double y, double *dx,
                double *dy) {
    struct wlr_scene_node *root;
    switch (layer) {
    case LAYER_FLOATING:
        root = &render->tree_floating->node;
        break;
    case LAYER_INSTANCE:
        root = &render->tree_instances->node;
        break;
    default:
        ww_unreachable();
    }

    struct wlr_scene_node *node = wlr_scene_node_at(root, x, y, dx, dy);
    if (node && node->type == WLR_SCENE_NODE_BUFFER && node->data) {
        return node->data;
    }
    return NULL;
}

static struct window *
window_from_xwl(struct comp_render *render, struct xwl_window *xwl_window) {
    struct window *window;
    wl_list_for_each (window, &render->windows, link) {
        if (window->xwl_window == xwl_window) {
            return window;
        }
    }
    return NULL;
}

static void
on_xwl_window_map(struct wl_listener *listener, void *data) {
    struct comp_render *render = wl_container_of(listener, render, on_xwl_window_map);
    struct xwl_window *xwl_window = data;

    struct window *window = window_from_xwl(render, xwl_window);
    if (!window) {
        window = calloc(1, sizeof(struct window));
        ww_assert(window);
        window->render = render;
        window->xwl_window = xwl_window;
        window->xwl_window->floating = true; // floating until taken off of tree_unknown

        wl_list_insert(&render->windows, &window->link);

        window->tree = wlr_scene_tree_create(render->tree_unknown);
        ww_assert(window->tree);
        window->scene_window = scene_window_create(window->tree, xwl_window->surface->surface);
        ww_assert(window->scene_window);
        window->scene_window->buffer->node.data = window;
    }

    if (window->tree->node.parent == render->tree_unknown) {
        wlr_scene_node_set_enabled(&window->tree->node, false);
    } else {
        wlr_scene_node_set_enabled(&window->tree->node, true);
    }

    wl_signal_emit_mutable(&render->events.window_map, window);
}

static void
on_xwl_window_unmap(struct wl_listener *listener, void *data) {
    struct comp_render *render = wl_container_of(listener, render, on_xwl_window_unmap);
    struct xwl_window *xwl_window = data;

    struct window *window = window_from_xwl(render, xwl_window);
    if (!window) {
        wlr_log(WLR_DEBUG, "on_xwl_window_unmap: no matching window (xwl: %p)", xwl_window);
        return;
    }

    wlr_scene_node_set_enabled(&window->tree->node, false);

    wl_signal_emit_mutable(&render->events.window_unmap, window);
}

static void
on_xwl_window_destroy(struct wl_listener *listener, void *data) {
    struct comp_render *render = wl_container_of(listener, render, on_xwl_window_destroy);
    struct xwl_window *xwl_window = data;

    struct window *window = window_from_xwl(render, xwl_window);
    if (!window) {
        wlr_log(WLR_DEBUG, "on_xwl_window_destroy: no matching window (xwl: %p)", xwl_window);
        return;
    }

    wl_signal_emit_mutable(&render->events.window_destroy, window);

    wl_list_remove(&window->link);
    wlr_scene_node_destroy(&window->tree->node);
    free(window);
}

static void
on_output_frame(struct wl_listener *listener, void *data) {
    struct output *output = wl_container_of(listener, output, on_frame);
    wlr_scene_output_commit(output->scene, NULL);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(output->scene, &now);
}

static void
on_output_request_state(struct wl_listener *listener, void *data) {
    struct output *output = wl_container_of(listener, output, on_request_state);
    const struct wlr_output_event_request_state *event = data;

    wlr_output_commit_state(output->wlr_output, event->state);
    if (!output->headless) {
        wl_signal_emit_mutable(&output->render->events.wl_output_resize, output);
    }
}

static void
on_output_destroy(struct wl_listener *listener, void *data) {
    struct output *output = wl_container_of(listener, output, on_destroy);

    wl_list_remove(&output->on_frame.link);
    wl_list_remove(&output->on_request_state.link);
    wl_list_remove(&output->on_destroy.link);
    wl_list_remove(&output->link);

    if (output->headless) {
        output->render->hl = NULL;
    } else {
        output->render->wl = NULL;
        wlr_log(WLR_INFO, "wayland output destroyed");

        wl_signal_emit_mutable(&output->render->events.wl_output_destroy, output);
    }

    free(output);
}

static void
on_new_output(struct wl_listener *listener, void *data) {
    struct comp_render *render = wl_container_of(listener, render, on_new_output);
    struct wlr_output *wlr_output = data;
    struct wlr_output_state state;

    wlr_output_init_render(wlr_output, render->compositor->allocator, render->compositor->renderer);
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    // Modesetting is not needed since we do not support the DRM backend.
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    struct output *output = calloc(1, sizeof(struct output));
    ww_assert(output);

    output->render = render;
    output->wlr_output = wlr_output;
    output->headless = wlr_output_is_headless(wlr_output);

    output->on_frame.notify = on_output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->on_frame);

    output->on_request_state.notify = on_output_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->on_request_state);

    output->on_destroy.notify = on_output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->on_destroy);

    // Add to output list
    wl_list_insert(&render->outputs, &output->link);

    // Set output up in scene coordinate space
    int x = output->headless ? OUTPUT_HL_X : OUTPUT_WL_X;
    int y = output->headless ? OUTPUT_HL_Y : OUTPUT_WL_Y;
    output->layout = wlr_output_layout_add(render->layout, wlr_output, x, y);
    ww_assert(output->layout);
    output->scene = wlr_scene_output_create(render->scene, wlr_output);
    ww_assert(output->scene);
    wlr_scene_output_layout_add_output(render->scene_layout, output->layout, output->scene);

    // Perform additional setup
    if (output->headless) {
        ww_assert(!render->hl);
        render->hl = output;
    } else {
        ww_assert(!render->wl);
        render->wl = output;

        output->remote.surface = wlr_wl_output_get_surface(wlr_output);
        ww_assert(output->remote.surface);

        wl_signal_emit_mutable(&render->events.wl_output_create, output);
    }
}

/*
 *  Internal API
 */

struct comp_render *
render_create(struct compositor *compositor) {
    struct comp_render *render = calloc(1, sizeof(struct comp_render));
    ww_assert(render);
    render->compositor = compositor;
    render->xwl = compositor->xwl;

    wl_list_init(&render->windows);

    // Xwayland
    render->on_xwl_window_map.notify = on_xwl_window_map;
    wl_signal_add(&render->xwl->events.window_map, &render->on_xwl_window_map);

    render->on_xwl_window_unmap.notify = on_xwl_window_unmap;
    wl_signal_add(&render->xwl->events.window_unmap, &render->on_xwl_window_unmap);

    render->on_xwl_window_destroy.notify = on_xwl_window_destroy;
    wl_signal_add(&render->xwl->events.window_destroy, &render->on_xwl_window_destroy);

    // Outputs
    render->layout = wlr_output_layout_create();
    ww_assert(render->layout);
    wl_list_init(&render->outputs);

    render->on_new_output.notify = on_new_output;
    wl_signal_add(&compositor->backend->events.new_output, &render->on_new_output);

    // Scene tree
    render->scene = wlr_scene_create();
    ww_assert(render->scene);

    render->tree_floating = wlr_scene_tree_create(&render->scene->tree);
    ww_assert(render->tree_floating);
    wlr_scene_node_set_position(&render->tree_floating->node, OUTPUT_WL_X, OUTPUT_WL_Y);

    render->tree_locks = wlr_scene_tree_create(&render->scene->tree);
    ww_assert(render->tree_locks);
    wlr_scene_node_set_position(&render->tree_locks->node, OUTPUT_WL_X, OUTPUT_WL_Y);

    render->tree_instances = wlr_scene_tree_create(&render->scene->tree);
    ww_assert(render->tree_instances);
    wlr_scene_node_set_position(&render->tree_instances->node, OUTPUT_WL_X, OUTPUT_WL_Y);

    render->tree_headless = wlr_scene_tree_create(&render->scene->tree);
    ww_assert(render->tree_headless);
    wlr_scene_node_set_position(&render->tree_headless->node, OUTPUT_HL_X, OUTPUT_HL_Y);

    render->tree_unknown = wlr_scene_tree_create(&render->scene->tree);
    ww_assert(render->tree_unknown);
    wlr_scene_node_set_enabled(&render->tree_unknown->node, false);

    wlr_scene_node_raise_to_top(&render->tree_floating->node);
    wlr_scene_node_place_below(&render->tree_locks->node, &render->tree_floating->node);
    wlr_scene_node_place_below(&render->tree_instances->node, &render->tree_locks->node);
    wlr_scene_node_place_below(&render->tree_headless->node, &render->tree_instances->node);
    wlr_scene_node_lower_to_bottom(&render->tree_unknown->node);

    render->background =
        wlr_scene_rect_create(&render->scene->tree, 16384, 16384,
                              (const float *)&render->compositor->config.background_color);
    ww_assert(render->background);
    wlr_scene_node_lower_to_bottom(&render->background->node);

    render->scene_layout = wlr_scene_attach_output_layout(render->scene, render->layout);
    ww_assert(render->scene_layout);

    wl_signal_init(&render->events.wl_output_create);
    wl_signal_init(&render->events.wl_output_resize);
    wl_signal_init(&render->events.wl_output_destroy);
    wl_signal_init(&render->events.window_map);
    wl_signal_init(&render->events.window_unmap);
    wl_signal_init(&render->events.window_destroy);

    return render;
}

void
render_destroy(struct comp_render *render) {
    struct output *output, *tmp;
    wl_list_for_each_safe (output, tmp, &render->outputs, link) {
        wlr_output_destroy(output->wlr_output);
    }
    wlr_output_layout_destroy(render->layout);

    wl_list_remove(&render->on_xwl_window_map.link);
    wl_list_remove(&render->on_xwl_window_unmap.link);
    wl_list_remove(&render->on_xwl_window_destroy.link);
    wl_list_remove(&render->on_new_output.link);

    wlr_scene_node_destroy(&render->scene->tree.node);
    free(render);
}

/*
 *  Public API
 */

void
render_focus_window(struct comp_render *render, struct window *window) {
    wlr_scene_node_raise_to_top(&window->tree->node);
}

void
render_layer_set_enabled(struct comp_render *render, enum window_layer layer, bool enabled) {
    switch (layer) {
    case LAYER_FLOATING:
        wlr_scene_node_set_enabled(&render->tree_floating->node, enabled);
        break;
    case LAYER_INSTANCE:
        wlr_scene_node_set_enabled(&render->tree_instances->node, enabled);
        break;
    case LAYER_LOCKS:
        wlr_scene_node_set_enabled(&render->tree_locks->node, enabled);
        break;
    default:
        ww_unreachable();
    }
}

void
render_output_get_size(struct output *output, int *w, int *h) {
    if (!output->wlr_output) {
        *w = *h = 0;
        return;
    }
    *w = output->wlr_output->width;
    *h = output->wlr_output->height;
}

void
render_recreate_output(struct comp_render *render) {
    if (render->wl) {
        return;
    }
    wlr_wl_output_create(render->compositor->backend_wl);
}

void
render_rect_configure(render_rect_t *rect, struct wlr_box box) {
    wlr_scene_node_set_position(&rect->node, box.x, box.y);
    wlr_scene_rect_set_size(rect, box.width, box.height);
}

render_rect_t *
render_rect_create(struct comp_render *render, struct wlr_box box, float color[4]) {
    render_rect_t *rect = wlr_scene_rect_create(render->tree_locks, box.width, box.height, color);
    ww_assert(rect);

    wlr_scene_node_set_position(&rect->node, box.x, box.y);
    return rect;
}

void
render_rect_set_color(render_rect_t *rect, float color[4]) {
    wlr_scene_rect_set_color(rect, color);
}

void
render_rect_set_enabled(render_rect_t *rect, bool enabled) {
    wlr_scene_node_set_enabled(&rect->node, enabled);
}

struct window *
render_window_at(struct comp_render *render, uint32_t layers, double x, double y, double *dx,
                 double *dy) {
    if (layers & LAYER_FLOATING) {
        struct window *window = window_at_layer(render, LAYER_FLOATING, x, y, dx, dy);
        if (window) {
            return window;
        }
    }

    if (layers & LAYER_INSTANCE) {
        struct window *window = window_at_layer(render, LAYER_INSTANCE, x, y, dx, dy);
        if (window) {
            return window;
        }
    }

    return NULL;
}

void
render_window_configure(struct window *window, int x, int y, int w, int h) {
    wlr_scene_node_set_position(&window->tree->node, x, y);
    xwl_window_configure(window->xwl_window, (struct wlr_box){x, y, w, h});
}

void
render_window_get_pos(struct window *window, int *x, int *y) {
    *x = window->tree->node.x;
    *y = window->tree->node.y;
}

void
render_window_get_size(struct window *window, int *w, int *h) {
    *w = window->xwl_window->surface->width;
    *h = window->xwl_window->surface->height;
}

void
render_window_set_dest_size(struct window *window, int w, int h) {
    scene_window_set_dest_size(window->scene_window, w, h);
}

void
render_window_set_enabled(struct window *window, bool enabled) {
    if (window->xwl_window->mapped) {
        wlr_scene_node_set_enabled(&window->tree->node, enabled);
    }
}

void
render_window_set_layer(struct window *window, enum window_layer layer) {
    switch (layer) {
    case LAYER_FLOATING:
        wlr_scene_node_reparent(&window->tree->node, window->render->tree_floating);
        break;
    case LAYER_INSTANCE:
        wlr_scene_node_reparent(&window->tree->node, window->render->tree_instances);
        break;
    default:
        ww_unreachable();
    }

    window->xwl_window->floating = layer == LAYER_FLOATING;
}

void
render_window_set_pos(struct window *window, int x, int y) {
    wlr_scene_node_set_position(&window->tree->node, x, y);
    xwl_window_configure(window->xwl_window, (struct wlr_box){
                                                 .x = x,
                                                 .y = y,
                                                 .width = window->xwl_window->surface->width,
                                                 .height = window->xwl_window->surface->height,
                                             });
}
