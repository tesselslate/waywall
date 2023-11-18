#ifndef WAYWALL_COMPOSITOR_SCENE_WINDOW_H
#define WAYWALL_COMPOSITOR_SCENE_WINDOW_H

#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/addon.h>

struct scene_window {
    struct wlr_scene_buffer *buffer;
    struct wlr_surface *surface;
    struct wlr_fbox src;
    int dest_width, dest_height;

    struct wlr_addon addon;
    struct wl_listener outputs_update;
    struct wl_listener output_enter;
    struct wl_listener output_leave;
    struct wl_listener output_sample;
    struct wl_listener frame_done;
    struct wl_listener surface_destroy;
    struct wl_listener surface_commit;
};

struct scene_window *scene_window_create(struct wlr_scene_tree *parent,
                                         struct wlr_surface *surface);
struct scene_window *scene_window_try_from_buffer(struct wlr_scene_buffer *scene_buffer);
void scene_window_set_src(struct scene_window *scene_window, struct wlr_fbox src);
void scene_window_set_dest_size(struct scene_window *scene_window, int width, int height);

#endif
