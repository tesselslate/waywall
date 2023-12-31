#ifndef WAYWALL_WALL_H
#define WAYWALL_WALL_H

#include "compositor/render.h"
#include "config.h"
#include "instance.h"
#include "layout.h"
#include "util.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>

/*
 *  wall contains state for the user-facing "wall."
 */
struct wall {
    // Instances
    int active_instance;
    struct instance instances[MAX_INSTANCES];
    struct wall_instance_data {
        struct hview *hview_chunkmap, *hview_instance;

        enum cpu_group last_group;
        bool locked;
    } instance_data[MAX_INSTANCES];
    size_t instance_count;
    bool alt_res;

    // Miscellaneous components
    struct reset_counter *reset_counter;

    // Layout
    struct layout layout;
    render_rect_t **rectangles;
    size_t rectangle_count;
    int screen_width, screen_height;

    // Input handling
    struct {
        int cx, cy;
        xkb_mod_mask_t modifiers;
        bool buttons[16]; // BTN_JOYSTICK - BTN_MOUSE

        struct {
            int x, y;
            struct keybind *bind;
        } last_bind;
    } input;

    // Input events
    struct wl_listener on_button;
    struct wl_listener on_key;
    struct wl_listener on_modifiers;
    struct wl_listener on_motion;

    // Render events
    struct wl_listener on_output_resize;
    struct wl_listener on_window_map;
    struct wl_listener on_window_unmap;
};

/*
 *  wall_create attempts to create a wall instance. Other globals (g_compositor, g_config, and
 *  g_inotify) must be present.
 */
struct wall *wall_create();

/*
 *  wall_destroy frees all resources associated with the wall object.
 */
void wall_destroy(struct wall *wall);

/*
 *  Inspects the given inotify_event and processes it if appropriate. Returns whether or not the
 *  event was processed by the wall.
 */
bool wall_process_inotify(struct wall *wall, const struct inotify_event *event);

/*
 *  Attempts to load a new configuration.
 */
bool wall_update_config(struct wall *wall);

#endif
