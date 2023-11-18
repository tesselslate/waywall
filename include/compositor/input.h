#ifndef WAYWALL_COMPOSITOR_INPUT_H
#define WAYWALL_COMPOSITOR_INPUT_H

#include "compositor/compositor.h"

/*
 *  comp_input contains most of the state related to user input, minus some remote Wayland globals
 *  which are stored in the compositor.
 *  Depends on the render subsystem.
 */
struct comp_input {
    struct compositor *compositor;
    struct comp_render *render;

    struct wl_listener on_window_unmap;

    double sens;
    bool on_wall;

    struct wlr_xcursor_manager *cursor_manager;
    struct wlr_cursor *cursor;
    struct wl_listener on_cursor_motion;
    struct wl_listener on_cursor_motion_absolute;
    struct wl_listener on_cursor_button;
    struct wl_listener on_cursor_axis;
    struct wl_listener on_cursor_frame;

    struct wlr_seat *seat;
    struct wl_list keyboards; // keyboard.link
    struct wl_listener on_new_input;
    struct wl_listener on_request_set_cursor;
    struct wl_listener on_request_set_selection;

    struct window *focused_window;
    struct window *grabbed_window;
    double grab_x, grab_y;

    struct wlr_pointer_constraints_v1 *pointer_constraints;
    struct wlr_pointer_constraint_v1 *active_constraint;
    struct wl_listener on_new_constraint;
    struct wl_listener on_wl_output_create;
    struct wl_listener on_wl_output_resize;
    struct wl_listener on_wl_output_destroy;

    struct wlr_relative_pointer_manager_v1 *relative_pointer;
    double acc_x, acc_y;

    struct {
        struct wl_signal button;    // data: compositor_button_event (stack allocated)
        struct wl_signal key;       // data: compositor_key_event (stack allocated)
        struct wl_signal modifiers; // data: xkb_mod_mask_t (stack allocated)
        struct wl_signal motion;    // data: compositor_motion_event (stack allocated)
    } events;
};

/*
 *  constraint contains state for a single pointer constraint (either locked or confined pointer.)
 *  We should only really have to handle locking the pointer to the center of the screen, as that
 *  is what Minecraft does.
 */
struct constraint {
    struct comp_input *input;
    struct wlr_pointer_constraint_v1 *wlr;

    struct wl_listener on_set_region;
    struct wl_listener on_destroy;
};

/*
 *  keyboard contains state for a particular keyboard. There currently do not need to be multiple
 *  keyboards, but there may be if/when multi-seat support is added.
 */
struct keyboard {
    struct wl_list link; // comp_input.keyboards

    struct comp_input *input;
    struct wlr_keyboard *wlr;

    struct wl_listener on_key;
    struct wl_listener on_modifiers;
    struct wl_listener on_destroy;
};

struct compositor_button_event {
    uint32_t button;
    uint32_t time_msec;
    bool state;
};

struct compositor_key_event {
    const xkb_keysym_t *syms;
    int nsyms;
    uint32_t modifiers;
    uint32_t time_msec;
    bool state;

    bool consumed;
};

struct compositor_motion_event {
    double x, y;
    uint32_t time_msec;
};

struct synthetic_key {
    uint8_t keycode;
    bool state;
};

/*
 *  Sends a synthetic mouse click to the given window.
 */
void input_click(struct window *window);

/*
 *  Attempts to set up input handling functionality for the compositor.
 */
struct comp_input *input_create(struct compositor *compositor);

/*
 *  Frees any resources allocated by the input subsystem.
 */
void input_destroy(struct comp_input *input);

/*
 *  Switches focus to the given window. If window is NULL, focus is removed from the currently
 *  focused window (if any).
 */
void input_focus_window(struct comp_input *input, struct window *window);

/*
 *  Notifies the input module of when a layer's visiblity has been toggled. Used to handle some
 *  annoying edge cases with pointer behavior.
 */
void input_layer_toggled(struct comp_input *input);

/*
 *  Applies a new configuration.
 */
void input_load_config(struct comp_input *input, struct compositor_config config);

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
