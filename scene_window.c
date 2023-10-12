#include "scene_window.h"
#include "util.h"
#include <stdlib.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_compositor.h>

/*
 *  This implementation is largely lifted from wlroots' code, with a few tweaks.
 *
 *  Copyright (c) 2017, 2018 Drew DeVault
 *  Copyright (c) 2014 Jari Vetoniemi
 *  Copyright (c) 2023 The wlroots contributors
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy of
 *  this software and associated documentation files (the "Software"), to deal in
 *  the Software without restriction, including without limitation the rights to
 *  use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 *  of the Software, and to permit persons to whom the Software is furnished to do
 *  so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 */

/*
 *  TODO: Figure out if the lack of linux dmabuf feedback matters. It's handled by
 *  scene_bufer_send_dmabuf_feedback in wlroots.
 */

// Taken from types/scene/wlr_scene.c
static struct wlr_scene *
scene_node_get_root(struct wlr_scene_node *node) {
    struct wlr_scene_tree *tree;
    if (node->type == WLR_SCENE_NODE_TREE) {
        tree = wlr_scene_tree_from_node(node);
    } else {
        tree = node->parent;
    }
    while (tree->node.parent != NULL) {
        tree = tree->node.parent;
    }
    struct wlr_scene *scene = wl_container_of(tree, scene, tree);
    return scene;
}

static bool
fbox_intersect(struct wlr_fbox *dest, struct wlr_fbox *a, struct wlr_fbox *b) {
    bool a_empty = wlr_fbox_empty(a);
    bool b_empty = wlr_fbox_empty(b);

    if (a_empty || b_empty) {
        *dest = (struct wlr_fbox){0};
        return false;
    }

    double x1 = fmax(a->x, b->x);
    double x2 = fmin(a->x + a->width, b->x + b->width);
    double y1 = fmax(a->y, b->y);
    double y2 = fmin(a->y + a->height, b->y + b->height);

    dest->x = x1;
    dest->y = y1;
    dest->width = x2 - x1;
    dest->height = y2 - y1;

    return !wlr_fbox_empty(dest);
}

static void
handle_scene_buffer_outputs_update(struct wl_listener *listener, void *data) {
    struct scene_window *window = wl_container_of(listener, window, outputs_update);

    if (window->buffer->primary_output == NULL) {
        return;
    }
    double scale = window->buffer->primary_output->output->scale;
    wlr_fractional_scale_v1_notify_scale(window->surface, scale);
    wlr_surface_set_preferred_buffer_scale(window->surface, ceil(scale));
}

static void
handle_scene_buffer_output_enter(struct wl_listener *listener, void *data) {
    struct scene_window *window = wl_container_of(listener, window, output_enter);
    struct wlr_scene_output *output = data;

    wlr_surface_send_enter(window->surface, output->output);
}

static void
handle_scene_buffer_output_leave(struct wl_listener *listener, void *data) {
    struct scene_window *window = wl_container_of(listener, window, output_leave);
    struct wlr_scene_output *output = data;

    wlr_surface_send_leave(window->surface, output->output);
}

static void
handle_scene_buffer_output_sample(struct wl_listener *listener, void *data) {
    struct scene_window *window = wl_container_of(listener, window, output_sample);
    const struct wlr_scene_output_sample_event *event = data;
    struct wlr_scene_output *scene_output = event->output;
    if (window->buffer->primary_output != scene_output) {
        return;
    }

    struct wlr_scene *root = scene_node_get_root(&window->buffer->node);
    if (!root->presentation) {
        return;
    }

    if (event->direct_scanout) {
        wlr_presentation_surface_scanned_out_on_output(root->presentation, window->surface,
                                                       scene_output->output);
    } else {
        wlr_presentation_surface_textured_on_output(root->presentation, window->surface,
                                                    scene_output->output);
    }
}

static void
handle_scene_buffer_frame_done(struct wl_listener *listener, void *data) {
    struct scene_window *window = wl_container_of(listener, window, frame_done);
    struct timespec *now = data;

    wlr_surface_send_frame_done(window->surface, now);
}

static void
scene_window_handle_surface_destroy(struct wl_listener *listener, void *data) {
    struct scene_window *window = wl_container_of(listener, window, surface_destroy);

    wlr_scene_node_destroy(&window->buffer->node);
}

static void
client_buffer_mark_next_can_damage(struct wlr_client_buffer *buffer) {
    buffer->n_ignore_locks++;
}

static void
scene_buffer_unmark_client_buffer(struct wlr_scene_buffer *scene_buffer) {
    if (!scene_buffer->buffer) {
        return;
    }

    struct wlr_client_buffer *buffer = wlr_client_buffer_get(scene_buffer->buffer);
    if (!buffer) {
        return;
    }

    ww_assert(buffer->n_ignore_locks > 0);
    buffer->n_ignore_locks--;
}

static void
set_buffer_with_surface_state(struct wlr_scene_buffer *scene_buffer, struct scene_window *window) {
    struct wlr_surface_state *state = &window->surface->current;

    wlr_scene_buffer_set_opaque_region(scene_buffer, &window->surface->opaque_region);

    struct wlr_fbox src_box;
    wlr_surface_get_buffer_source_box(window->surface, &src_box);
    struct wlr_fbox final_src_box;
    if (!fbox_intersect(&final_src_box, &src_box, &window->src)) {
        final_src_box = src_box;
    }
    wlr_scene_buffer_set_source_box(scene_buffer, &final_src_box);

    if (window->dest_width > 0 || window->dest_height > 0) {
        wlr_scene_buffer_set_dest_size(scene_buffer, window->dest_width, window->dest_height);
    } else {
        wlr_scene_buffer_set_dest_size(scene_buffer, state->width, state->height);
    }
    wlr_scene_buffer_set_transform(scene_buffer, state->transform);

    scene_buffer_unmark_client_buffer(scene_buffer);

    if (window->buffer) {
        client_buffer_mark_next_can_damage(window->surface->buffer);

        wlr_scene_buffer_set_buffer_with_damage(scene_buffer, &window->surface->buffer->base,
                                                &window->surface->buffer_damage);
    } else {
        wlr_scene_buffer_set_buffer(scene_buffer, NULL);
    }
}

static void
handle_scene_window_surface_commit(struct wl_listener *listener, void *data) {
    struct scene_window *window = wl_container_of(listener, window, surface_commit);
    struct wlr_scene_buffer *scene_buffer = window->buffer;

    set_buffer_with_surface_state(scene_buffer, window);

    int lx, ly;
    bool enabled = wlr_scene_node_coords(&scene_buffer->node, &lx, &ly);

    if (!wl_list_empty(&window->surface->current.frame_callback_list) &&
        window->buffer->primary_output != NULL && enabled) {
        wlr_output_schedule_frame(window->buffer->primary_output->output);
    }
}

static bool
scene_buffer_point_accepts_input(struct wlr_scene_buffer *scene_buffer, int sx, int sy) {
    struct scene_window *window = scene_window_try_from_buffer(scene_buffer);

    return wlr_surface_point_accepts_input(window->surface, sx, sy);
}

static void
window_addon_destroy(struct wlr_addon *addon) {
    struct scene_window *window = wl_container_of(addon, window, addon);

    scene_buffer_unmark_client_buffer(window->buffer);

    wlr_addon_finish(&window->addon);

    wl_list_remove(&window->outputs_update.link);
    wl_list_remove(&window->output_enter.link);
    wl_list_remove(&window->output_leave.link);
    wl_list_remove(&window->output_sample.link);
    wl_list_remove(&window->frame_done.link);
    wl_list_remove(&window->surface_destroy.link);
    wl_list_remove(&window->surface_commit.link);

    free(window);
}

static const struct wlr_addon_interface window_addon_impl = {
    .name = "scene_window",
    .destroy = window_addon_destroy,
};

struct scene_window *
scene_window_create(struct wlr_scene_tree *parent, struct wlr_surface *surface) {
    struct scene_window *window = calloc(1, sizeof(struct scene_window));
    ww_assert(window);

    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_create(parent, NULL);
    if (!scene_buffer) {
        free(window);
        return NULL;
    }

    window->buffer = scene_buffer;
    window->surface = surface;
    scene_buffer->point_accepts_input = scene_buffer_point_accepts_input;

    window->outputs_update.notify = handle_scene_buffer_outputs_update;
    wl_signal_add(&scene_buffer->events.outputs_update, &window->outputs_update);

    window->output_enter.notify = handle_scene_buffer_output_enter;
    wl_signal_add(&scene_buffer->events.output_enter, &window->output_enter);

    window->output_leave.notify = handle_scene_buffer_output_leave;
    wl_signal_add(&scene_buffer->events.output_leave, &window->output_leave);

    window->output_sample.notify = handle_scene_buffer_output_sample;
    wl_signal_add(&scene_buffer->events.output_sample, &window->output_sample);

    window->frame_done.notify = handle_scene_buffer_frame_done;
    wl_signal_add(&scene_buffer->events.frame_done, &window->frame_done);

    window->surface_destroy.notify = scene_window_handle_surface_destroy;
    wl_signal_add(&surface->events.destroy, &window->surface_destroy);

    window->surface_commit.notify = handle_scene_window_surface_commit;
    wl_signal_add(&surface->events.commit, &window->surface_commit);

    wlr_addon_init(&window->addon, &scene_buffer->node.addons, scene_buffer, &window_addon_impl);

    set_buffer_with_surface_state(scene_buffer, window);

    return window;
}

struct scene_window *
scene_window_try_from_buffer(struct wlr_scene_buffer *scene_buffer) {
    struct wlr_addon *addon =
        wlr_addon_find(&scene_buffer->node.addons, scene_buffer, &window_addon_impl);
    if (!addon) {
        return NULL;
    }

    struct scene_window *window = wl_container_of(addon, window, addon);
    return window;
}

void
scene_window_set_src(struct scene_window *scene_window, struct wlr_fbox src) {
    scene_window->src = src;
    set_buffer_with_surface_state(scene_window->buffer, scene_window);
}

void
scene_window_set_dest_size(struct scene_window *scene_window, int width, int height) {
    scene_window->dest_width = width;
    scene_window->dest_height = height;
    set_buffer_with_surface_state(scene_window->buffer, scene_window);
}
