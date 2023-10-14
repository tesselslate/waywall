#ifndef WAYWALL_COMPOSITOR_PUB_INPUT_H
#define WAYWALL_COMPOSITOR_PUB_INPUT_H

#include "compositor/pub_render.h"

struct comp_input;

#ifndef WAYWALL_COMPOSITOR_IMPL

struct comp_input {
    struct {
        struct wl_signal button;    // data: compositor_button_event (stack allocated)
        struct wl_signal modifiers; // data: xkb_mod_mask_t (stack allocated)
        struct wl_signal motion;    // data: compositor_motion_event (stack allocated)
    } events;

    bool (*key_callback)(struct compositor_key_event event);
};

#endif

/*
 *  Sends a synthetic mouse click to the given window.
 */
void input_click(struct window *window);

/*
 *  Switches focus to the given window. If window is NULL, focus is removed from the currently
 *  focused window (if any).
 */
void input_focus_window(struct comp_input *input, struct window *window);

/*
 *  Sends a sequence of synthetic key events to the given window.
 */
void input_send_keys(struct window *window, const struct synthetic_key *keys, size_t count);

/*
 *  Notify the input subsystem of whether the user is on the wall or not.
 */
void input_set_on_wall(struct comp_input *input, bool state);

/*
 *  Sets the mouse sensitivity for 3D cursor motion.
 */
void input_set_sensitivity(struct comp_input *input, double sens);

#endif
