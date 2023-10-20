/*
 *  The input module is responsible for handling user input. It processes mouse and keyboard inputs
 *  as well as other related functionality like pointer constraints.
 */

#define WAYWALL_COMPOSITOR_IMPL

#include "compositor/input.h"
#include "compositor/render.h"
#include "compositor/xwayland.h"
#include "relative-pointer-unstable-v1-protocol.h"
#include "util.h"
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/backend.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

/*
 *  Keyboard events
 */

// TODO: Fix cursor image changing during window moving (weird Ninjabrain Bot behavior, most likely
//       need to report accurate cursor position)

static uint32_t
get_layer_mask(struct comp_input *input) {
    if (input->on_wall) {
        return LAYER_FLOATING;
    }

    uint32_t mask = LAYER_INSTANCE;
    if (!input->active_constraint) {
        mask |= LAYER_FLOATING;
    }
    return mask;
}

static void
on_keyboard_key(struct wl_listener *listener, void *data) {
    struct keyboard *keyboard = wl_container_of(listener, keyboard, on_key);
    struct wlr_keyboard_key_event *event = data;

    // Convert from libinput -> XKB
    uint32_t keycode = event->keycode + 8;

    // Get a list of keysyms for this keycode without taking modifiers into account.
    // Source: river (Mapping.zig:75)
    const xkb_keysym_t *syms;
    struct xkb_keymap *keymap = xkb_state_get_keymap(keyboard->wlr->xkb_state);
    xkb_layout_index_t index = xkb_state_key_get_layout(keyboard->wlr->xkb_state, keycode);
    int nsyms = xkb_keymap_key_get_syms_by_level(keymap, keycode, index, 0, &syms);

    struct compositor_key_event comp_event = {
        .syms = syms,
        .nsyms = nsyms,
        .modifiers = wlr_keyboard_get_modifiers(keyboard->wlr),
        .state = event->state == WL_KEYBOARD_KEY_STATE_PRESSED,
        .time_msec = event->time_msec,
    };

    // If the wall module does not eat the keyboard input, we can send it along.
    ww_assert(keyboard->input->key_callback);
    if (!keyboard->input->key_callback(comp_event)) {
        wlr_seat_set_keyboard(keyboard->input->seat, keyboard->wlr);
        wlr_seat_keyboard_notify_key(keyboard->input->seat, event->time_msec, event->keycode,
                                     event->state);
    }
}

static void
on_keyboard_modifiers(struct wl_listener *listener, void *data) {
    struct keyboard *keyboard = wl_container_of(listener, keyboard, on_modifiers);

    wlr_seat_set_keyboard(keyboard->input->seat, keyboard->wlr);
    wlr_seat_keyboard_notify_modifiers(keyboard->input->seat, &keyboard->wlr->modifiers);

    wl_signal_emit_mutable(&keyboard->input->events.modifiers, &keyboard->wlr->modifiers.depressed);
}

static void
on_keyboard_destroy(struct wl_listener *listener, void *data) {
    struct keyboard *keyboard = wl_container_of(listener, keyboard, on_destroy);

    wl_list_remove(&keyboard->on_key.link);
    wl_list_remove(&keyboard->on_modifiers.link);
    wl_list_remove(&keyboard->on_destroy.link);
    wl_list_remove(&keyboard->link);
    free(keyboard);
}

static void
handle_new_keyboard(struct comp_input *input, struct wlr_input_device *device) {
    struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

    struct keyboard *keyboard = calloc(1, sizeof(struct keyboard));
    ww_assert(keyboard);

    keyboard->input = input;
    keyboard->wlr = wlr_keyboard;

    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(wlr_keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(ctx);

    wlr_keyboard_set_repeat_info(wlr_keyboard, input->compositor->config.repeat_rate,
                                 input->compositor->config.repeat_delay);

    keyboard->on_key.notify = on_keyboard_key;
    wl_signal_add(&wlr_keyboard->events.key, &keyboard->on_key);

    keyboard->on_modifiers.notify = on_keyboard_modifiers;
    wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->on_modifiers);

    keyboard->on_destroy.notify = on_keyboard_destroy;
    wl_signal_add(&device->events.destroy, &keyboard->on_destroy);

    wlr_seat_set_keyboard(input->seat, wlr_keyboard);

    wl_list_insert(&input->keyboards, &keyboard->link);
}

/*
 *  Pointer events
 */

static void
handle_new_pointer(struct comp_input *input, struct wlr_input_device *device) {
    wlr_cursor_attach_input_device(input->cursor, device);
}

static void
handle_cursor_motion(struct comp_input *input, uint32_t time_msec) {
    // If there is an ongoing interactive move, handle that and return.
    if (input->grabbed_window) {
        double x = input->cursor->x - input->grab_x;
        double y = input->cursor->y - input->grab_y;
        render_window_set_pos(input->grabbed_window, x, y);
        return;
    }

    // Figure out which window to give pointer focus to. Pointer focus can change based on where
    // the cursor moves.
    double dx, dy;
    struct window *window = render_window_at(input->render, get_layer_mask(input), input->cursor->x,
                                             input->cursor->y, &dx, &dy);

    if (window) {
        wlr_seat_pointer_notify_enter(input->seat, window->xwl_window->surface->surface, dx, dy);
        wlr_seat_pointer_notify_motion(input->seat, time_msec, dx, dy);
    } else if (input->on_wall) {
        // If there is no window with pointer focus, we want to set the cursor image.
        wlr_cursor_set_xcursor(input->cursor, input->cursor_manager, "default");
        wlr_seat_pointer_notify_clear_focus(input->seat);

        // Notify the wall module of the mouse movement.
        struct compositor_motion_event event = {
            .x = input->cursor->x,
            .y = input->cursor->y,
            .time_msec = time_msec,
        };
        wl_signal_emit_mutable(&input->events.motion, &event);
    }
}

static void
on_cursor_motion(struct wl_listener *listener, void *data) {
    struct comp_input *input = wl_container_of(listener, input, on_cursor_motion);
    struct wlr_pointer_motion_event *event = data;

    // Update the cursor position and then do further processing.
    wlr_cursor_move(input->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    handle_cursor_motion(input, event->time_msec);
}

static void
on_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    struct comp_input *input = wl_container_of(listener, input, on_cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;

    // Map the cursor to the Wayland output. Assume that the Wayland output is the only output which
    // cursor events can ever access.
    ww_assert(input->render->wl);
    wlr_cursor_map_input_to_output(input->cursor, &event->pointer->base,
                                   input->render->wl->wlr_output);

    // Update the cursor position and then do further processing.
    wlr_cursor_warp_absolute(input->cursor, &event->pointer->base, event->x, event->y);
    handle_cursor_motion(input, event->time_msec);
}

static void
on_cursor_button(struct wl_listener *listener, void *data) {
    struct comp_input *input = wl_container_of(listener, input, on_cursor_button);
    struct wlr_pointer_button_event *event = data;

    struct compositor_button_event comp_event = {
        .button = event->button,
        .time_msec = event->time_msec,
        .state = event->state == WLR_BUTTON_PRESSED,
    };

    // If the event is for a button release, notify any interested parties. In particular, the wall
    // module needs to know when buttons are released, even when a window is focused.
    if (event->state == WLR_BUTTON_RELEASED) {
        wl_signal_emit_mutable(&input->events.button, &comp_event);
    }

    // If there is an active pointer constraint, we don't want to do any further processing of
    // button events. Just give them to the focused Minecraft instance.
    if (input->active_constraint) {
        wlr_seat_pointer_notify_button(input->seat, event->time_msec, event->button, event->state);
        return;
    }

    // Otherwise, we need to handle floating window functionality - click to focus and interactive
    // window moving.
    if (event->state == WLR_BUTTON_RELEASED) {
        // If the user was moving a window and released the left mouse button, stop the interactive
        // move.
        if (input->grabbed_window && event->button == BTN_LEFT) {
            input->grabbed_window = NULL;
            return;
        }

        // If there was no interactive move taking place, pass the release event to the window
        // with pointer focus.
        wlr_seat_pointer_notify_button(input->seat, event->time_msec, event->button, event->state);
        return;
    }

    ww_assert(event->state == WLR_BUTTON_PRESSED);

    // If there is an active window grab, do not pass through button events to the grabbed window.
    if (input->grabbed_window) {
        return;
    }

    static const uint32_t MOVE_MODMASK = WLR_MODIFIER_SHIFT;
    bool held_move_mods =
        (input->seat->keyboard_state.keyboard->modifiers.depressed & MOVE_MODMASK) == MOVE_MODMASK;

    // Try to start an interactive move. If it fails, proceed as normal.
    if (held_move_mods && event->button == BTN_LEFT) {
        ww_assert(!input->grabbed_window);

        struct window *window = render_window_at(input->render, LAYER_FLOATING, input->cursor->x,
                                                 input->cursor->y, &input->grab_x, &input->grab_y);
        if (window) {
            // Switch focus to the grabbed window so events are sent to it after the interactive
            // move is done.
            input_focus_window(input, window);
            input->grabbed_window = window;

            return;
        }
    }

    // Update the focused window.
    struct window *window = render_window_at(input->render, get_layer_mask(input), input->cursor->x,
                                             input->cursor->y, NULL, NULL);

    if (window) {
        input_focus_window(input, window);
    } else {
        // We will only take focus away from *all* windows if on the wall. If we are in an instance,
        // we don't want to let the user click on the background (e.g. during alt res) and unfocus
        // the instance.
        if (input->on_wall) {
            input_focus_window(input, NULL);
        }
    }

    // If there is a focused window, send it the button event. Otherwise, signal it to the wall
    // module.
    if (input->focused_window) {
        wlr_seat_pointer_notify_button(input->seat, event->time_msec, event->button, event->state);
    } else {
        wl_signal_emit_mutable(&input->events.button, &comp_event);
    }
}

static void
on_cursor_axis(struct wl_listener *listener, void *data) {
    struct comp_input *input = wl_container_of(listener, input, on_cursor_axis);
    struct wlr_pointer_axis_event *event = data;

    wlr_seat_pointer_notify_axis(input->seat, event->time_msec, event->orientation, event->delta,
                                 event->delta_discrete, event->source);
}

static void
on_cursor_frame(struct wl_listener *listener, void *data) {
    struct comp_input *input = wl_container_of(listener, input, on_cursor_frame);

    wlr_seat_pointer_notify_frame(input->seat);
}

static void
on_relative_motion(void *data, struct zwp_relative_pointer_v1 *relative_pointer, uint32_t utime_hi,
                   uint32_t utime_lo, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t dx_unaccel,
                   wl_fixed_t dy_unaccel) {
    struct comp_input *input = data;

    uint64_t time = (uint64_t)utime_hi * 0xFFFFFFFF + (uint64_t)utime_lo;

    // Boat eye relies on very precise cursor positioning ingame, and non-integer cursor motion
    // causes problems with that. Hence, we want to accumulate any cursor motion and only notify
    // Xwayland of cursor motion in roughly whole pixel increments.
    input->acc_x += wl_fixed_to_double(dx_unaccel) * input->sens;
    input->acc_y += wl_fixed_to_double(dy_unaccel) * input->sens;

    double x = trunc(input->acc_x);
    input->acc_x -= x;

    double y = trunc(input->acc_y);
    input->acc_y -= y;

    wlr_relative_pointer_manager_v1_send_relative_motion(input->relative_pointer, input->seat, time,
                                                         wl_fixed_to_double(dx),
                                                         wl_fixed_to_double(dy), x, y);
}

static const struct zwp_relative_pointer_v1_listener relative_pointer_listener = {
    .relative_motion = on_relative_motion,
};

/*
 *  Pointer constraints
 */

static void
derestrict_pointer(struct comp_input *input) {
    struct output *wl = input->render->wl;
    if (!wl) {
        return;
    }

    if (wl->remote.confined_pointer) {
        zwp_confined_pointer_v1_destroy(wl->remote.confined_pointer);
        wl->remote.confined_pointer = NULL;
    }
    if (wl->remote.locked_pointer) {
        zwp_locked_pointer_v1_destroy(wl->remote.locked_pointer);
        wl->remote.locked_pointer = NULL;

        // If the user's compositor respects the unlock hint, their cursor will be put at these
        // coordinates. However, we don't receive a motion event for it, so we need to warp the
        // cursor image to the center ourselves.
        wlr_cursor_warp(input->cursor, NULL, wl->wlr_output->width / 2, wl->wlr_output->height / 2);

        // TODO: Sending a fake handle motion event here to update the cursor position tracked in
        // the wall module doesn't work. This will need to be improved.
    }
}

static void
confine_pointer(struct comp_input *input) {
    derestrict_pointer(input);

    struct output *wl = input->render->wl;
    if (!wl) {
        return;
    }

    wl->remote.confined_pointer = zwp_pointer_constraints_v1_confine_pointer(
        input->compositor->remote.constraints, wl->remote.surface,
        input->compositor->remote.pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    ww_assert(wl->remote.confined_pointer);
}

static void
lock_pointer(struct comp_input *input) {
    derestrict_pointer(input);

    struct output *wl = input->render->wl;
    if (!wl) {
        return;
    }

    wl->remote.locked_pointer = zwp_pointer_constraints_v1_lock_pointer(
        input->compositor->remote.constraints, wl->remote.surface,
        input->compositor->remote.pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    ww_assert(wl->remote.locked_pointer);

    zwp_locked_pointer_v1_set_cursor_position_hint(wl->remote.locked_pointer,
                                                   wl_fixed_from_int(wl->wlr_output->width / 2),
                                                   wl_fixed_from_int(wl->wlr_output->height / 2));
}

static void
handle_constraint(struct comp_input *input, struct wlr_pointer_constraint_v1 *constraint) {
    if (input->active_constraint == constraint) {
        // We do not care if the constraint gets updated.
        return;
    }

    // Deactivate the previous constraint, if any.
    if (input->active_constraint) {
        wlr_pointer_constraint_v1_send_deactivated(input->active_constraint);
    }
    input->active_constraint = constraint;

    // Confine or unrestrict the pointer, depending on the user's config.
    if (!constraint) {
        if (input->compositor->config.confine_pointer) {
            confine_pointer(input);
        } else {
            derestrict_pointer(input);
        }
        return;
    }

    // If the new constraint is not owned by the focused window, do not handle it.
    if (input->focused_window &&
        input->focused_window->xwl_window->surface->surface != constraint->surface) {
        return;
    }

    lock_pointer(input);
    wlr_pointer_constraint_v1_send_activated(constraint);
}

static void
on_constraint_set_region(struct wl_listener *listener, void *data) {
    // We do not care about whatever properties pointer constraints may have, we simply assume that
    // all constraints are Minecraft requesting to lock the pointer to the output's center.
}

static void
on_constraint_destroy(struct wl_listener *listener, void *data) {
    struct wlr_pointer_constraint_v1 *wlr_constraint = data;
    struct constraint *constraint = wlr_constraint->data;
    struct comp_input *input = constraint->input;

    if (wlr_constraint == input->active_constraint) {
        // If the constraint is still active, deactivate it.
        handle_constraint(input, NULL);
    }

    wl_list_remove(&constraint->on_set_region.link);
    wl_list_remove(&constraint->on_destroy.link);
    free(constraint);
}

static void
on_new_constraint(struct wl_listener *listener, void *data) {
    struct comp_input *input = wl_container_of(listener, input, on_new_constraint);
    struct wlr_pointer_constraint_v1 *wlr_constraint = data;

    struct constraint *constraint = calloc(1, sizeof(struct constraint));
    ww_assert(constraint);

    wlr_constraint->data = constraint;
    constraint->input = input;
    constraint->wlr = wlr_constraint;

    constraint->on_set_region.notify = on_constraint_set_region;
    wl_signal_add(&wlr_constraint->events.set_region, &constraint->on_set_region);

    constraint->on_destroy.notify = on_constraint_destroy;
    wl_signal_add(&wlr_constraint->events.destroy, &constraint->on_destroy);

    // If the constraint is owned by the focused window (active Minecraft instance), handle it
    // immediately.
    if (input->focused_window &&
        input->focused_window->xwl_window->surface->surface == wlr_constraint->surface) {
        handle_constraint(input, wlr_constraint);
    }
}

static void
on_wl_output_create(struct wl_listener *listener, void *data) {
    struct comp_input *input = wl_container_of(listener, input, on_wl_output_create);
    struct output *wl_output = data;

    ww_assert(!wl_output->remote.confined_pointer);
    ww_assert(!wl_output->remote.locked_pointer);
    ww_assert(wl_output->remote.surface);

    if (input->active_constraint) {
        lock_pointer(input);
    } else {
        if (input->compositor->config.confine_pointer) {
            confine_pointer(input);
        } else {
            derestrict_pointer(input);
        }
    }
}

static void
on_wl_output_resize(struct wl_listener *listener, void *data) {
    struct output *wl_output = data;

    // We need to update the cursor position hint to reflect the new output size, so that the cursor
    // is still warped to the center of the remote window on the next unlock.
    if (wl_output->remote.locked_pointer) {
        zwp_locked_pointer_v1_set_cursor_position_hint(
            wl_output->remote.locked_pointer, wl_fixed_from_int(wl_output->wlr_output->width / 2),
            wl_fixed_from_int(wl_output->wlr_output->height / 2));
    }
}

static void
on_wl_output_destroy(struct wl_listener *listener, void *data) {
    struct output *wl = data;

    if (wl->remote.confined_pointer) {
        zwp_confined_pointer_v1_destroy(wl->remote.confined_pointer);
    }
    if (wl->remote.locked_pointer) {
        zwp_locked_pointer_v1_destroy(wl->remote.locked_pointer);
    }
}

/*
 *  Seat events
 */

static void
on_request_set_cursor(struct wl_listener *listener, void *data) {
    struct comp_input *input = wl_container_of(listener, input, on_request_set_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;

    // Only allow clients with pointer focus to change the curosr image.
    struct wlr_seat_client *focused_client = input->seat->pointer_state.focused_client;
    if (focused_client == event->seat_client) {
        wlr_cursor_set_surface(input->cursor, event->surface, event->hotspot_x, event->hotspot_y);
    }
}

static void
on_request_set_selection(struct wl_listener *listener, void *data) {
    struct comp_input *input = wl_container_of(listener, input, on_request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;

    wlr_seat_set_selection(input->seat, event->source, event->serial);
}

static void
on_new_input(struct wl_listener *listener, void *data) {
    struct comp_input *input = wl_container_of(listener, input, on_new_input);
    struct wlr_input_device *device = data;

    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        handle_new_keyboard(input, device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        handle_new_pointer(input, device);
        break;
    default:
        wlr_log(WLR_INFO, "unknown input device of type %d (name '%s')", (int)device->type,
                device->name);
        break;
    }

    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&input->keyboards)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(input->seat, caps);
}

static void
on_window_unmap(struct wl_listener *listener, void *data) {
    struct comp_input *input = wl_container_of(listener, input, on_window_unmap);
    struct window *window = data;

    if (input->grabbed_window == window) {
        input->grabbed_window = NULL;
    }

    // If the focused window was unmapped, we need to refocus the appropriate window.
    if (input->focused_window != window) {
        return;
    }

    if (input->on_wall) {
        input_focus_window(input, NULL);
    } else {
        // Focus the topmost instance.
        if (!input->render->wl) {
            input_focus_window(input, NULL);
            return;
        }

        struct window *window = render_window_at(
            input->render, LAYER_INSTANCE, input->render->wl->wlr_output->width / 2,
            input->render->wl->wlr_output->height / 2, NULL, NULL);
        input_focus_window(input, window);
    }
}

/*
 *  Internal API
 */

struct comp_input *
input_create(struct compositor *compositor) {
    ww_assert(compositor->render);

    struct comp_input *input = calloc(1, sizeof(struct comp_input));
    ww_assert(input);
    input->compositor = compositor;
    input->render = compositor->render;

    input->on_window_unmap.notify = on_window_unmap;
    wl_signal_add(&compositor->render->events.window_unmap, &input->on_window_unmap);

    // Cursor (pointer)
    input->cursor_manager =
        wlr_xcursor_manager_create(compositor->config.cursor_theme, compositor->config.cursor_size);
    ww_assert(input->cursor_manager);

    input->cursor = wlr_cursor_create();
    ww_assert(input->cursor);
    wlr_cursor_attach_output_layout(input->cursor, input->render->layout);

    input->on_cursor_motion.notify = on_cursor_motion;
    wl_signal_add(&input->cursor->events.motion, &input->on_cursor_motion);

    input->on_cursor_motion_absolute.notify = on_cursor_motion_absolute;
    wl_signal_add(&input->cursor->events.motion_absolute, &input->on_cursor_motion_absolute);

    input->on_cursor_button.notify = on_cursor_button;
    wl_signal_add(&input->cursor->events.button, &input->on_cursor_button);

    input->on_cursor_axis.notify = on_cursor_axis;
    wl_signal_add(&input->cursor->events.axis, &input->on_cursor_axis);

    input->on_cursor_frame.notify = on_cursor_frame;
    wl_signal_add(&input->cursor->events.frame, &input->on_cursor_frame);

    // Pointer constraints
    ww_assert(compositor->remote.constraints);
    input->pointer_constraints = wlr_pointer_constraints_v1_create(compositor->display);
    ww_assert(input->pointer_constraints);

    input->on_new_constraint.notify = on_new_constraint;
    wl_signal_add(&input->pointer_constraints->events.new_constraint, &input->on_new_constraint);

    input->on_wl_output_create.notify = on_wl_output_create;
    wl_signal_add(&compositor->render->events.wl_output_create, &input->on_wl_output_create);

    input->on_wl_output_resize.notify = on_wl_output_resize;
    wl_signal_add(&compositor->render->events.wl_output_resize, &input->on_wl_output_resize);

    input->on_wl_output_destroy.notify = on_wl_output_destroy;
    wl_signal_add(&compositor->render->events.wl_output_destroy, &input->on_wl_output_destroy);

    // Relative pointer
    input->relative_pointer = wlr_relative_pointer_manager_v1_create(compositor->display);
    ww_assert(input->relative_pointer);

    ww_assert(compositor->remote.relative_pointer);
    zwp_relative_pointer_v1_add_listener(compositor->remote.relative_pointer,
                                         &relative_pointer_listener, input);

    // Seat
    input->seat = wlr_seat_create(compositor->display, "seat0");
    ww_assert(input->seat);

    struct wlr_data_device_manager *data_device_manager =
        wlr_data_device_manager_create(compositor->display);
    ww_assert(data_device_manager);

    wl_list_init(&input->keyboards);

    input->on_request_set_cursor.notify = on_request_set_cursor;
    wl_signal_add(&input->seat->events.request_set_cursor, &input->on_request_set_cursor);

    input->on_request_set_selection.notify = on_request_set_selection;
    wl_signal_add(&input->seat->events.request_set_selection, &input->on_request_set_selection);

    input->on_new_input.notify = on_new_input;
    wl_signal_add(&compositor->backend->events.new_input, &input->on_new_input);

    // Events
    wl_signal_init(&input->events.button);
    wl_signal_init(&input->events.modifiers);
    wl_signal_init(&input->events.motion);

    return input;
}

void
input_destroy(struct comp_input *input) {
    wlr_xcursor_manager_destroy(input->cursor_manager);
    wlr_cursor_destroy(input->cursor);
    wlr_seat_destroy(input->seat);
    free(input);
}

void
input_load_config(struct comp_input *input, struct compositor_config config) {
    struct keyboard *keyboard;
    wl_list_for_each (keyboard, &input->keyboards, link) {
        wlr_keyboard_set_repeat_info(keyboard->wlr, config.repeat_rate, config.repeat_delay);
    }

    // Confine or unrestrict the pointer as needed.
    bool diff_confine = config.confine_pointer != input->compositor->config.confine_pointer;
    if (diff_confine && !input->active_constraint) {
        if (config.confine_pointer) {
            confine_pointer(input);
        } else {
            derestrict_pointer(input);
        }
    }

    struct wlr_xcursor_manager *cursor_manager =
        wlr_xcursor_manager_create(config.cursor_theme, config.cursor_size);
    if (!cursor_manager) {
        wlr_log(WLR_ERROR, "failed to create new cursor manager");
    } else {
        wlr_xcursor_manager_destroy(input->cursor_manager);
        input->cursor_manager = cursor_manager;
        xwl_update_cursor(input->compositor->xwl);

        // Update the cursor image if needed. This isn't fully correct (the user may be hovering
        // over Ninjabrain Bot) but it's close enough for now. TODO: Improve.
        if (!input->active_constraint) {
            wlr_cursor_set_xcursor(input->cursor, input->cursor_manager, "default");
        }
    }
}

/*
 *  Public API
 */

void
input_click(struct window *window) {
    xwl_click(window->xwl_window);
}

void
input_focus_window(struct comp_input *input, struct window *window) {
    if (window == input->focused_window) {
        return;
    }

    // If the window focus changes, we do not want to continue any ongoing interactive move.
    // When an interactive move is started, this function is called before grabbed_window is set,
    // so this is fine.
    input->grabbed_window = NULL;

    if (window) {
        struct wlr_surface *surface = window->xwl_window->surface->surface;
        render_focus_window(input->render, window);
        xwl_window_activate(window->xwl_window);

        // Handle keyboard focus.
        struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(input->seat);
        if (keyboard) {
            wlr_seat_keyboard_notify_enter(input->seat, surface, keyboard->keycodes,
                                           keyboard->num_keycodes, &keyboard->modifiers);
        }

        // Handle pointer focus.
        int x, y;
        render_window_get_pos(window, &x, &y);
        wlr_seat_pointer_notify_enter(input->seat, surface, input->cursor->x - (double)x,
                                      input->cursor->y - (double)y);

        // Handle pointer constraints.
        struct wlr_pointer_constraint_v1 *constraint =
            wlr_pointer_constraints_v1_constraint_for_surface(input->pointer_constraints, surface,
                                                              input->seat);
        handle_constraint(input, constraint);
    } else {
        handle_constraint(input, NULL);
        wlr_seat_keyboard_notify_clear_focus(input->seat);
        wlr_seat_pointer_notify_clear_focus(input->seat);
        xwl_window_deactivate(input->focused_window->xwl_window);

        // The cursor image will not be updated automatically until the user moves their mouse, so
        // we update it again here.
        wlr_cursor_set_xcursor(input->cursor, input->cursor_manager, "default");
    }

    input->focused_window = window;
}

void
input_send_keys(struct window *window, const struct synthetic_key *keys, size_t count) {
    xwl_send_keys(window->xwl_window, keys, count);
}

void
input_set_on_wall(struct comp_input *input, bool state) {
    input->on_wall = state;
}

void
input_set_sensitivity(struct comp_input *input, double sens) {
    input->sens = sens;
}
