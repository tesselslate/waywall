/*
 *  The hview module provides facilities for creating and managing "headless views," which are
 *  copies of windows that appear on the verification (headless) output. Headless views can be
 *  cropped and stretched, and there may be multiple headless views for a given window.
 *
 *  The owner of an hview is responsible for destroying that hview after its parent window has
 *  died.
 */

#include "compositor/hview.h"
#include "compositor/render.h"
#include "compositor/scene_window.h"
#include "compositor/xwayland.h"
#include "util.h"
#include <stdlib.h>
#include <wlr/xwayland.h>

struct hview {
    struct window *window;
    struct scene_window *scene_window;
    struct wlr_box src, dest;
    bool enabled;

    struct wl_listener on_window_map;
    struct wl_listener on_window_unmap;
    struct wl_listener on_window_destroy;
};

static void
configure_hview(struct hview *hview) {
    ww_assert(hview->scene_window);

    scene_window_set_src(
        hview->scene_window,
        (struct wlr_fbox){hview->src.x, hview->src.y, hview->src.width, hview->src.height});

    wlr_scene_node_set_position(&hview->scene_window->buffer->node, hview->dest.x, hview->dest.y);
    scene_window_set_dest_size(hview->scene_window, hview->dest.width, hview->dest.height);

    wlr_scene_node_set_enabled(&hview->scene_window->buffer->node, hview->enabled);
}

static void
map_hview(struct hview *hview) {
    ww_assert(!hview->scene_window);

    hview->scene_window = scene_window_create(hview->window->render->tree_headless,
                                              hview->window->xwl_window->surface->surface);
    ww_assert(hview->scene_window);
    configure_hview(hview);
}

static void
handle_window_map(struct wl_listener *listener, void *data) {
    struct hview *hview = wl_container_of(listener, hview, on_window_map);

    if (!hview->scene_window && hview->window) {
        map_hview(hview);
    }
}

static void
handle_window_unmap(struct wl_listener *listener, void *data) {
    struct hview *hview = wl_container_of(listener, hview, on_window_unmap);

    wlr_scene_node_destroy(&hview->scene_window->buffer->node);
    hview->scene_window = NULL;
}

static void
handle_window_destroy(struct wl_listener *listener, void *data) {
    struct hview *hview = wl_container_of(listener, hview, on_window_destroy);

    wl_list_remove(&hview->on_window_map.link);
    wl_list_remove(&hview->on_window_unmap.link);
    wl_list_remove(&hview->on_window_destroy.link);

    hview->window = NULL;
}

struct hview *
hview_create(struct window *window) {
    ww_assert(window);

    struct hview *hview = calloc(1, sizeof(struct hview));
    ww_assert(hview);

    hview->window = window;
    hview->enabled = true;

    hview->on_window_map.notify = handle_window_map;
    wl_signal_add(&window->xwl_window->events.map, &hview->on_window_map);

    hview->on_window_unmap.notify = handle_window_unmap;
    wl_signal_add(&window->xwl_window->events.unmap, &hview->on_window_unmap);

    hview->on_window_destroy.notify = handle_window_destroy;
    wl_signal_add(&window->xwl_window->events.destroy, &hview->on_window_destroy);

    if (window->xwl_window->mapped) {
        map_hview(hview);
    }

    return hview;
}

void
hview_destroy(struct hview *hview) {
    ww_assert(hview);

    if (hview->scene_window) {
        wlr_scene_node_destroy(&hview->scene_window->buffer->node);
    }
    if (hview->window) {
        wl_list_remove(&hview->on_window_map.link);
        wl_list_remove(&hview->on_window_unmap.link);
        wl_list_remove(&hview->on_window_destroy.link);
    }
    free(hview);
}

void
hview_raise(struct hview *hview) {
    ww_assert(hview);

    wlr_scene_node_raise_to_top(&hview->scene_window->buffer->node);
}

void
hview_set_dest(struct hview *hview, struct wlr_box box) {
    ww_assert(hview);

    hview->dest = box;
    if (hview->scene_window) {
        configure_hview(hview);
    }
}

void
hview_set_enabled(struct hview *hview, bool enabled) {
    ww_assert(hview);

    hview->enabled = enabled;
    if (hview->scene_window) {
        configure_hview(hview);
    }
}

void
hview_set_src(struct hview *hview, struct wlr_box box) {
    ww_assert(hview);

    hview->src = box;
    if (hview->scene_window) {
        configure_hview(hview);
    }
}
