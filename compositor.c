#include "compositor.h"
#include "pointer-constraints-unstable-v1-protocol.h"
#include "relative-pointer-unstable-v1-protocol.h"
#include "util.h"
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/wayland.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <wlr/xwayland/xwayland.h>
#include <xcb/xproto.h>
#include <xkbcommon/xkbcommon.h>

#define WL_X 0
#define WL_Y 0
#define HEADLESS_X 16384
#define HEADLESS_Y 16384

// HACK: Any reason xwm_destroy isn't called already by wlr_xwayland_destroy? Maybe ask wlroots
// people about this.
extern void xwm_destroy(struct wlr_xwm *xwm);

struct compositor {
    struct wl_display *display;
    struct wlr_allocator *allocator;
    struct wlr_backend *backend;
    struct wlr_backend *backend_wl;
    struct wlr_backend *backend_headless;
    struct wlr_compositor *compositor;
    struct wlr_renderer *renderer;
    struct wlr_export_dmabuf_manager_v1 *dmabuf_export;

    struct wlr_scene *scene;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_scene_rect *background;

    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_manager;
    double mouse_sens;
    struct wl_listener on_cursor_motion;
    struct wl_listener on_cursor_motion_absolute;
    struct wl_listener on_cursor_button;
    struct wl_listener on_cursor_axis;
    struct wl_listener on_cursor_frame;

    struct wlr_seat *seat;
    struct wl_list keyboards;
    struct wl_listener on_new_input;
    struct wl_listener on_request_cursor;
    struct wl_listener on_request_set_selection;

    struct wlr_output_layout *output_layout;
    struct wl_list outputs;
    struct wl_listener on_new_output;
    struct output *wl_output;
    struct output *headless_output;

    struct wlr_xwayland *xwayland;
    struct xcb_connection_t *xcb;
    struct wl_list windows;
    struct window *focused_window;
    struct wl_listener on_xwayland_new_surface;
    struct wl_listener on_xwayland_ready;

    struct wl_display *remote_display;
    struct wl_pointer *remote_pointer;
    struct wl_seat *remote_seat;

    struct wlr_pointer_constraints_v1 *pointer_constraints;
    struct wlr_pointer_constraint_v1 *active_constraint;
    struct zwp_pointer_constraints_v1 *remote_pointer_constraints;
    struct zwp_locked_pointer_v1 *remote_locked_pointer;
    struct zwp_confined_pointer_v1 *remote_confined_pointer;
    struct wl_listener on_new_constraint;

    struct wlr_relative_pointer_manager_v1 *relative_pointer;
    struct zwp_relative_pointer_manager_v1 *remote_relative_pointer_manager;
    struct zwp_relative_pointer_v1 *remote_relative_pointer;

    struct compositor_config config;
    struct compositor_vtable vtable;
    bool should_stop;
};

struct keyboard {
    struct wl_list link;

    struct compositor *compositor;
    struct wlr_keyboard *wlr_keyboard;

    struct wl_listener on_modifiers;
    struct wl_listener on_key;
    struct wl_listener on_destroy;
};

struct output {
    struct wl_list link;

    struct compositor *compositor;
    struct wlr_output *wlr_output;
    struct wlr_output_layout_output *layout;
    struct wlr_scene_output *scene;

    bool headless;
    struct wl_surface *remote_surface;

    struct wl_listener on_frame;
    struct wl_listener on_request_state;
    struct wl_listener on_destroy;
};

struct pointer_constraint {
    struct compositor *compositor;
    struct wlr_pointer_constraint_v1 *constraint;

    struct wl_listener on_set_region;
    struct wl_listener on_destroy;
};

struct window {
    struct wl_list link;

    struct compositor *compositor;
    struct wlr_xwayland_surface *surface;
    struct wlr_scene_tree *scene_tree;
    struct wlr_scene_surface *scene_surface;

    struct headless_view {
        struct wlr_scene_tree *tree;
        struct wlr_scene_surface *surface;
    } headless_views[4];
    int headless_view_count;

    struct wl_listener on_associate;
    struct wl_listener on_dissociate;

    struct wl_listener on_map;
    struct wl_listener on_unmap;
    struct wl_listener on_destroy;
    struct wl_listener on_request_activate;
    struct wl_listener on_request_configure;
    struct wl_listener on_request_fullscreen;
};

static void
global_to_surface(struct compositor *compositor, struct wlr_scene_node *node, double cx, double cy,
                  double *x, double *y) {
    int ix, iy;
    wlr_scene_node_coords(node, &ix, &iy);
    *x = cx - (double)ix;
    *y = cy - (double)iy;
}

static uint32_t
now_msec() {
    // HACK: For now Xwayland uses CLOCK_MONOTONIC and CLOCK_MONOTONIC uses some point near system
    // boot as its epoch. Hopefully this remains the case forever, since I don't want to replicate
    // the awful time calculation logic from resetti.

    // HACK: GLFW expects each keypress to have an ascending timestamp. We must make sure each
    // timestamp returned by this function is greater than the last.

    static uint32_t last_now = 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t ms = now.tv_sec * 1000 + now.tv_nsec / 1000000;
    if (ms <= last_now) {
        ms = ++last_now;
    }
    return (uint32_t)ms;
}

static bool
send_event(xcb_connection_t *xcb, xcb_window_t window, uint32_t mask, const char *event) {
    xcb_void_cookie_t cookie = xcb_send_event_checked(xcb, true, window, mask, event);
    xcb_generic_error_t *err = xcb_request_check(xcb, cookie);
    if (err) {
        int opcode = (int)(event[0]);
        wlr_log(WLR_ERROR, "failed to send event (opcode: %d): %d\n", opcode, err->error_code);
        free(err);
    }
    return err == NULL;
}

static void
handle_cursor_motion(struct compositor *compositor, uint32_t time_msec) {
    if (!compositor->focused_window) {
        wlr_seat_pointer_clear_focus(compositor->seat);
        return;
    }
    double x, y;
    global_to_surface(compositor, &compositor->focused_window->scene_tree->node,
                      compositor->cursor->x, compositor->cursor->y, &x, &y);
    wlr_seat_pointer_notify_enter(compositor->seat, compositor->focused_window->surface->surface, x,
                                  y);
    wlr_seat_pointer_notify_motion(compositor->seat, time_msec, x, y);
}

static void
on_registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
                   uint32_t version) {
    struct compositor *compositor = data;
    if (strcmp(interface, wl_seat_interface.name) == 0) {
        // TODO: multiseat
        if (compositor->remote_seat) {
            wlr_log(WLR_DEBUG, "extra seat advertised by compositor");
            return;
        }
        compositor->remote_seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        ww_assert(compositor->remote_seat);
        compositor->remote_pointer = wl_seat_get_pointer(compositor->remote_seat);
        ww_assert(compositor->remote_pointer);
    } else if (strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) {
        compositor->remote_pointer_constraints =
            wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1);
        ww_assert(compositor->remote_pointer_constraints);
    } else if (strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
        compositor->remote_relative_pointer_manager =
            wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1);
        ww_assert(compositor->remote_relative_pointer_manager);
    }
}

static void
on_registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    // TODO: Handle seat destruction?
    wlr_log(WLR_INFO, "waywall: global %" PRIu32 "removed", name);
}

static struct wl_registry_listener registry_listener = {
    .global = on_registry_global,
    .global_remove = on_registry_global_remove,
};

static void
on_relative_pointer_motion(void *data, struct zwp_relative_pointer_v1 *relative_pointer,
                           uint32_t utime_hi, uint32_t utime_lo, wl_fixed_t dx, wl_fixed_t dy,
                           wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel) {
    struct compositor *compositor = data;
    uint64_t time = (uint64_t)utime_hi * 0xFFFFFFFF + (uint64_t)utime_lo;
    wlr_relative_pointer_manager_v1_send_relative_motion(
        compositor->relative_pointer, compositor->seat, time, wl_fixed_to_double(dx),
        wl_fixed_to_double(dy), wl_fixed_to_double(dx_unaccel) * compositor->mouse_sens,
        wl_fixed_to_double(dy_unaccel) * compositor->mouse_sens);
}

static struct zwp_relative_pointer_v1_listener relative_pointer_listener = {
    .relative_motion = on_relative_pointer_motion,
};

static void
on_cursor_axis(struct wl_listener *listener, void *data) {
    struct compositor *compositor = wl_container_of(listener, compositor, on_cursor_axis);
    struct wlr_pointer_axis_event *event = data;
    wlr_seat_pointer_notify_axis(compositor->seat, event->time_msec, event->orientation,
                                 event->delta, event->delta_discrete, event->source);
}

static void
on_cursor_button(struct wl_listener *listener, void *data) {
    struct compositor *compositor = wl_container_of(listener, compositor, on_cursor_button);
    struct wlr_pointer_button_event *wlr_event = data;
    struct compositor_button_event event = {
        .button = wlr_event->button,
        .time_msec = wlr_event->time_msec,
        .state = wlr_event->state == WLR_BUTTON_PRESSED,
    };
    if (!compositor->vtable.button(event)) {
        wlr_seat_pointer_notify_button(compositor->seat, wlr_event->time_msec, wlr_event->button,
                                       wlr_event->state);
    }
}

static void
on_cursor_frame(struct wl_listener *listener, void *data) {
    struct compositor *compositor = wl_container_of(listener, compositor, on_cursor_frame);
    wlr_seat_pointer_notify_frame(compositor->seat);
}

static void
on_cursor_motion(struct wl_listener *listener, void *data) {
    struct compositor *compositor = wl_container_of(listener, compositor, on_cursor_motion);
    struct wlr_pointer_motion_event *wlr_event = data;
    wlr_cursor_move(compositor->cursor, &wlr_event->pointer->base, wlr_event->delta_x,
                    wlr_event->delta_y);
    handle_cursor_motion(compositor, wlr_event->time_msec);
    struct compositor_motion_event event = {
        .x = compositor->cursor->x,
        .y = compositor->cursor->y,
        .time_msec = wlr_event->time_msec,
    };
    compositor->vtable.motion(event);
}

static void
on_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    struct compositor *compositor =
        wl_container_of(listener, compositor, on_cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *wlr_event = data;

    // We can assume that the pointer motion is being reported by the sole Wayland output and not
    // the headless output.
    ww_assert(compositor->wl_output);
    wlr_cursor_map_input_to_output(compositor->cursor, &wlr_event->pointer->base,
                                   compositor->wl_output->wlr_output);

    wlr_cursor_warp_absolute(compositor->cursor, &wlr_event->pointer->base, wlr_event->x,
                             wlr_event->y);
    handle_cursor_motion(compositor, wlr_event->time_msec);
}

static void
on_keyboard_destroy(struct wl_listener *listener, void *data) {
    struct keyboard *keyboard = wl_container_of(listener, keyboard, on_destroy);
    wl_list_remove(&keyboard->on_destroy.link);
    wl_list_remove(&keyboard->on_key.link);
    wl_list_remove(&keyboard->on_modifiers.link);
    wl_list_remove(&keyboard->link);
    free(keyboard);
}

static void
on_keyboard_key(struct wl_listener *listener, void *data) {
    struct keyboard *keyboard = wl_container_of(listener, keyboard, on_key);
    struct compositor *compositor = keyboard->compositor;
    struct wlr_keyboard_key_event *wlr_event = data;

    // libinput keycode -> xkbcommon keycode
    uint32_t keycode = wlr_event->keycode + 8;

    // We need to do whatever this is to avoid XKB changing keysyms based on modifiers.
    // I took this from river (Mapping.zig:75, c16628) so I have no idea what this does to be
    // honest.
    const xkb_keysym_t *syms;
    struct xkb_keymap *keymap = xkb_state_get_keymap(keyboard->wlr_keyboard->xkb_state);
    xkb_layout_index_t index = xkb_state_key_get_layout(keyboard->wlr_keyboard->xkb_state, keycode);
    int nsyms = xkb_keymap_key_get_syms_by_level(keymap, keycode, index, 0, &syms);

    struct compositor_key_event event = {
        .syms = syms,
        .nsyms = nsyms,
        .modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard),
        .state = wlr_event->state == WL_KEYBOARD_KEY_STATE_PRESSED,
        .time_msec = wlr_event->time_msec,
    };

    if (!compositor->vtable.key(event)) {
        wlr_seat_set_keyboard(compositor->seat, keyboard->wlr_keyboard);
        wlr_seat_keyboard_notify_key(compositor->seat, wlr_event->time_msec, wlr_event->keycode,
                                     wlr_event->state);
    }
}

static void
on_keyboard_modifiers(struct wl_listener *listener, void *data) {
    struct keyboard *keyboard = wl_container_of(listener, keyboard, on_modifiers);
    wlr_seat_set_keyboard(keyboard->compositor->seat, keyboard->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(keyboard->compositor->seat,
                                       &keyboard->wlr_keyboard->modifiers);
    keyboard->compositor->vtable.modifiers(keyboard->wlr_keyboard->modifiers.depressed);
}

static void
on_output_frame(struct wl_listener *listener, void *data) {
    struct output *output = wl_container_of(listener, output, on_frame);
    struct wlr_scene *scene = output->compositor->scene;
    struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(scene, output->wlr_output);
    wlr_scene_output_commit(scene_output, NULL);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

static void
on_output_request_state(struct wl_listener *listener, void *data) {
    struct output *output = wl_container_of(listener, output, on_request_state);
    const struct wlr_output_event_request_state *event = data;
    wlr_output_commit_state(output->wlr_output, event->state);

    if (!output->headless) {
        output->compositor->vtable.resize(output->wlr_output->width, output->wlr_output->height);
    }
}

static void
on_output_destroy(struct wl_listener *listener, void *data) {
    struct output *output = wl_container_of(listener, output, on_destroy);
    wl_list_remove(&output->on_destroy.link);
    wl_list_remove(&output->on_frame.link);
    wl_list_remove(&output->on_request_state.link);
    wl_list_remove(&output->link);

    if (!output->headless) {
        output->compositor->wl_output = NULL;
        wlr_log(WLR_INFO, "wayland output destroyed");
        if (output->compositor->config.stop_on_close) {
            wlr_log(WLR_INFO, "stopping compositor due to window closing");
            compositor_stop(output->compositor);
        }
    }
    free(output);
}

static void
on_new_output(struct wl_listener *listener, void *data) {
    struct compositor *compositor = wl_container_of(listener, compositor, on_new_output);
    struct wlr_output *wlr_output = data;
    struct wlr_output_state state;

    wlr_output_init_render(wlr_output, compositor->allocator, compositor->renderer);
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    // No modesetting is necessary since we only use the Wayland and headless backends.
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    struct output *output = calloc(1, sizeof(struct output));
    ww_assert(output);
    output->compositor = compositor;
    output->wlr_output = wlr_output;
    output->headless = wlr_output_is_headless(wlr_output);

    output->on_frame.notify = on_output_frame;
    output->on_request_state.notify = on_output_request_state;
    output->on_destroy.notify = on_output_destroy;
    wl_signal_add(&wlr_output->events.frame, &output->on_frame);
    wl_signal_add(&wlr_output->events.request_state, &output->on_request_state);
    wl_signal_add(&wlr_output->events.destroy, &output->on_destroy);
    wl_list_insert(&compositor->outputs, &output->link);

    int coord = output->headless ? HEADLESS_X : 0;
    output->layout = wlr_output_layout_add(compositor->output_layout, wlr_output, coord, coord);
    ww_assert(output->layout);
    output->scene = wlr_scene_output_create(compositor->scene, wlr_output);
    ww_assert(output->scene);
    wlr_scene_output_layout_add_output(compositor->scene_layout, output->layout, output->scene);

    if (!output->headless) {
        output->remote_surface = wlr_wl_output_get_surface(wlr_output);

        ww_assert(!compositor->wl_output);
        compositor->wl_output = output;

        if (!compositor->background) {
            compositor->background =
                wlr_scene_rect_create(&compositor->scene->tree, 16384, 16384,
                                      (const float *)&compositor->config.background_color);
            ww_assert(compositor->background);
            wlr_scene_node_lower_to_bottom(&compositor->background->node);
        }
    } else {
        ww_assert(!compositor->headless_output);
        compositor->headless_output = output;
    }
}

static void
on_new_keyboard(struct compositor *compositor, struct wlr_input_device *device) {
    struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);
    struct keyboard *keyboard = calloc(1, sizeof(struct keyboard));
    ww_assert(keyboard);
    keyboard->compositor = compositor;
    keyboard->wlr_keyboard = wlr_keyboard;

    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap =
        xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(wlr_keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(wlr_keyboard, compositor->config.repeat_rate,
                                 compositor->config.repeat_delay);

    keyboard->on_destroy.notify = on_keyboard_destroy;
    keyboard->on_key.notify = on_keyboard_key;
    keyboard->on_modifiers.notify = on_keyboard_modifiers;
    wl_signal_add(&device->events.destroy, &keyboard->on_destroy);
    wl_signal_add(&wlr_keyboard->events.key, &keyboard->on_key);
    wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->on_modifiers);

    wlr_seat_set_keyboard(compositor->seat, wlr_keyboard);
    wl_list_insert(&compositor->keyboards, &keyboard->link);
}

static void
on_new_pointer(struct compositor *compositor, struct wlr_input_device *device) {
    wlr_cursor_attach_input_device(compositor->cursor, device);
}

static void
on_new_input(struct wl_listener *listener, void *data) {
    struct compositor *compositor = wl_container_of(listener, compositor, on_new_input);
    struct wlr_input_device *device = data;
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        on_new_keyboard(compositor, device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        on_new_pointer(compositor, device);
        break;
    default:
        break;
    }

    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&compositor->keyboards)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(compositor->seat, caps);
}

static void
handle_constraint(struct compositor *compositor, struct wlr_pointer_constraint_v1 *constraint) {
    // Since all we care about is Minecraft, we can assume that all constraints keep the pointer
    // at the center of the screen.
    if (compositor->active_constraint == constraint) {
        return;
    }
    if (!compositor->wl_output) {
        return;
    }

    if (compositor->active_constraint) {
        wlr_pointer_constraint_v1_send_deactivated(compositor->active_constraint);
        if (!constraint) {
            ww_assert(compositor->remote_locked_pointer);
            zwp_locked_pointer_v1_destroy(compositor->remote_locked_pointer);
            compositor->remote_locked_pointer = NULL;
            compositor->active_constraint = NULL;
            if (compositor->config.confine_pointer) {
                compositor->remote_confined_pointer = zwp_pointer_constraints_v1_confine_pointer(
                    compositor->remote_pointer_constraints, compositor->wl_output->remote_surface,
                    compositor->remote_pointer, NULL,
                    ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
                ww_assert(compositor->remote_confined_pointer);
            }
            return;
        }
    }

    if (compositor->focused_window &&
        compositor->focused_window->surface->surface == constraint->surface) {
        int32_t width = compositor->wl_output->wlr_output->width;
        int32_t height = compositor->wl_output->wlr_output->height;
        wlr_cursor_warp(compositor->cursor, NULL, width / 2, height / 2);

        if (compositor->remote_confined_pointer) {
            zwp_confined_pointer_v1_destroy(compositor->remote_confined_pointer);
            compositor->remote_confined_pointer = NULL;
        }
        if (!compositor->remote_locked_pointer) {
            compositor->remote_locked_pointer = zwp_pointer_constraints_v1_lock_pointer(
                compositor->remote_pointer_constraints, compositor->wl_output->remote_surface,
                compositor->remote_pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
            ww_assert(compositor->remote_locked_pointer);
        }
        zwp_locked_pointer_v1_set_cursor_position_hint(compositor->remote_locked_pointer,
                                                       wl_fixed_from_int(width / 2),
                                                       wl_fixed_from_int(height / 2));
        wlr_pointer_constraint_v1_send_activated(constraint);
        compositor->active_constraint = constraint;
    }
}

static void
on_constraint_set_region(struct wl_listener *listener, void *data) {
    // We don't care about what the game wants to set the pointer constraints to.
}

static void
on_constraint_destroy(struct wl_listener *listener, void *data) {
    struct wlr_pointer_constraint_v1 *wlr_constraint = data;
    struct pointer_constraint *constraint = wlr_constraint->data;
    if (constraint->compositor->active_constraint == wlr_constraint) {
        handle_constraint(constraint->compositor, NULL);
    }
    wl_list_remove(&constraint->on_destroy.link);
    wl_list_remove(&constraint->on_set_region.link);
    free(constraint);
}

static void
on_new_constraint(struct wl_listener *listener, void *data) {
    struct compositor *compositor = wl_container_of(listener, compositor, on_new_constraint);
    struct wlr_pointer_constraint_v1 *wlr_constraint = data;
    struct pointer_constraint *constraint = calloc(1, sizeof(struct pointer_constraint));
    ww_assert(constraint);

    wlr_constraint->data = constraint;
    constraint->compositor = compositor;
    constraint->constraint = wlr_constraint;
    constraint->on_set_region.notify = on_constraint_set_region;
    constraint->on_destroy.notify = on_constraint_destroy;
    wl_signal_add(&wlr_constraint->events.set_region, &constraint->on_set_region);
    wl_signal_add(&wlr_constraint->events.destroy, &constraint->on_destroy);
    if (compositor->focused_window &&
        wlr_constraint->surface == compositor->focused_window->surface->surface) {
        handle_constraint(compositor, wlr_constraint);
    }
}

static void
on_request_cursor(struct wl_listener *listener, void *data) {
    struct compositor *compositor = wl_container_of(listener, compositor, on_request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    struct wlr_seat_client *focused = compositor->seat->pointer_state.focused_client;
    if (focused == event->seat_client) {
        wlr_cursor_set_surface(compositor->cursor, event->surface, event->hotspot_x,
                               event->hotspot_y);
    }
}

static void
on_request_set_selection(struct wl_listener *listener, void *data) {
    struct compositor *compositor = wl_container_of(listener, compositor, on_request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(compositor->seat, event->source, event->serial);
}

static void
on_window_associate(struct wl_listener *listener, void *data) {
    struct window *window = wl_container_of(listener, window, on_associate);
    wl_signal_add(&window->surface->surface->events.map, &window->on_map);
    wl_signal_add(&window->surface->surface->events.unmap, &window->on_unmap);
}

static void
on_window_dissociate(struct wl_listener *listener, void *data) {
    struct window *window = wl_container_of(listener, window, on_dissociate);
    wl_list_remove(&window->on_map.link);
    wl_list_remove(&window->on_unmap.link);
}

static void
on_window_map(struct wl_listener *listener, void *data) {
    struct window *window = wl_container_of(listener, window, on_map);
    wl_list_insert(&window->compositor->windows, &window->link);

    window->scene_tree = wlr_scene_tree_create(&window->compositor->scene->tree);
    wlr_scene_node_set_enabled(&window->scene_tree->node, true);
    window->scene_surface = wlr_scene_surface_create(window->scene_tree, window->surface->surface);
    wlr_scene_node_set_position(&window->scene_tree->node, WL_X, WL_Y);

    window->compositor->vtable.window(window, true);
}

static void
on_window_unmap(struct wl_listener *listener, void *data) {
    struct window *window = wl_container_of(listener, window, on_unmap);
    wl_list_remove(&window->link);
    wlr_scene_node_destroy(&window->scene_tree->node);
    compositor_window_destroy_headless_views(window);

    if (window == window->compositor->focused_window) {
        wlr_log(WLR_DEBUG, "focused window was unmapped");
        compositor_window_focus(window->compositor, NULL);
    }
    window->compositor->vtable.window(window, false);
}

static void
on_window_destroy(struct wl_listener *listener, void *data) {
    struct window *window = wl_container_of(listener, window, on_destroy);
    wl_list_remove(&window->on_associate.link);
    wl_list_remove(&window->on_dissociate.link);
    wl_list_remove(&window->on_destroy.link);
    wl_list_remove(&window->on_request_activate.link);
    wl_list_remove(&window->on_request_configure.link);
    wl_list_remove(&window->on_request_fullscreen.link);

    struct compositor *compositor = window->compositor;
    free(window);
    if (compositor->should_stop && wl_list_length(&compositor->windows) == 0) {
        wl_display_terminate(compositor->display);
    }
}

static void
on_window_request_activate(struct wl_listener *listener, void *data) {
    struct window *window = wl_container_of(listener, window, on_request_activate);
    wlr_log(WLR_DEBUG, "window %d requested activation", window->surface->window_id);
}

static void
on_window_request_configure(struct wl_listener *listener, void *data) {
    struct window *window = wl_container_of(listener, window, on_request_configure);
    wlr_log(WLR_DEBUG, "window %d requested configuration", window->surface->window_id);

    // TODO: Allow ninb to be resized (when that's implemented)
}

static void
on_window_request_fullscreen(struct wl_listener *listener, void *data) {
    struct window *window = wl_container_of(listener, window, on_request_fullscreen);
    wlr_log(WLR_DEBUG, "window %d requested fullscreen", window->surface->window_id);
}

static void
on_xwayland_new_surface(struct wl_listener *listener, void *data) {
    struct compositor *compositor = wl_container_of(listener, compositor, on_xwayland_new_surface);
    struct wlr_xwayland_surface *surface = data;
    if (surface->override_redirect) {
        wlr_log(WLR_INFO, "xwayland surface wants override redirect");
    }
    wlr_log(WLR_DEBUG, "window %d created", surface->window_id);

    struct window *window = calloc(1, sizeof(struct window));
    ww_assert(window);
    window->compositor = compositor;
    window->surface = surface;

    window->on_associate.notify = on_window_associate;
    window->on_dissociate.notify = on_window_dissociate;

    // The signals for map and unmap are handled by associate and dissociate.
    window->on_map.notify = on_window_map;
    window->on_unmap.notify = on_window_unmap;
    window->on_destroy.notify = on_window_destroy;
    window->on_request_activate.notify = on_window_request_activate;
    window->on_request_configure.notify = on_window_request_configure;
    window->on_request_fullscreen.notify = on_window_request_fullscreen;
    wl_signal_add(&surface->events.associate, &window->on_associate);
    wl_signal_add(&surface->events.dissociate, &window->on_dissociate);
    wl_signal_add(&surface->events.destroy, &window->on_destroy);
    wl_signal_add(&surface->events.request_activate, &window->on_request_activate);
    wl_signal_add(&surface->events.request_configure, &window->on_request_configure);
    wl_signal_add(&surface->events.request_fullscreen, &window->on_request_fullscreen);
}

static void
on_xwayland_ready(struct wl_listener *listener, void *data) {
    struct compositor *compositor = wl_container_of(listener, compositor, on_xwayland_ready);
    xcb_connection_t *conn = xcb_connect(NULL, NULL);
    int err = xcb_connection_has_error(conn);
    if (err) {
        wlr_log(WLR_ERROR, "failed to connect to xwayland: %d", err);
        return;
    }
    compositor->xcb = conn;
}

struct compositor *
compositor_create(struct compositor_vtable vtable, struct compositor_config config) {
    struct compositor *compositor = calloc(1, sizeof(struct compositor));
    if (!compositor) {
        wlr_log(WLR_ERROR, "failed to allocate compositor");
        return NULL;
    }
    compositor->config = config;

    ww_assert(vtable.button);
    ww_assert(vtable.key);
    ww_assert(vtable.motion);
    ww_assert(vtable.modifiers);
    ww_assert(vtable.resize);
    ww_assert(vtable.window);
    compositor->vtable = vtable;

    compositor->display = wl_display_create();
    if (!compositor->display) {
        wlr_log(WLR_ERROR, "failed to create wl_display");
        goto fail_display;
    }

    compositor->backend_headless = wlr_headless_backend_create(compositor->display);
    if (!compositor->backend_headless) {
        wlr_log(WLR_ERROR, "failed to create headless backend");
        goto fail_backend_headless;
    }
    // TODO: make verification output size configurable
    wlr_headless_add_output(compositor->backend_headless, HEADLESS_WIDTH, HEADLESS_HEIGHT);

    compositor->backend_wl = wlr_wl_backend_create(compositor->display, NULL);
    if (!compositor->backend_wl) {
        wlr_log(WLR_ERROR, "failed to create wayland backend");
        goto fail_backend_wl;
    }
    compositor->remote_display = wlr_wl_backend_get_remote_display(compositor->backend_wl);
    ww_assert(compositor->remote_display);
    struct wl_registry *remote_registry = wl_display_get_registry(compositor->remote_display);
    wl_registry_add_listener(remote_registry, &registry_listener, compositor);
    wl_display_roundtrip(compositor->remote_display);
    if (!compositor->remote_pointer) {
        wlr_log(WLR_ERROR, "failed to acquire remote pointer");
        goto fail_registry;
    }
    if (!compositor->remote_pointer_constraints) {
        wlr_log(WLR_ERROR, "failed to acquire remote pointer constraints");
        goto fail_registry;
    }
    if (!compositor->remote_relative_pointer_manager) {
        wlr_log(WLR_ERROR, "failed to acquire remote relative pointer manager");
        goto fail_registry;
    }
    compositor->remote_relative_pointer = zwp_relative_pointer_manager_v1_get_relative_pointer(
        compositor->remote_relative_pointer_manager, compositor->remote_pointer);
    ww_assert(compositor->remote_relative_pointer);
    zwp_relative_pointer_v1_add_listener(compositor->remote_relative_pointer,
                                         &relative_pointer_listener, compositor);
    wlr_wl_output_create(compositor->backend_wl);

    compositor->backend = wlr_multi_backend_create(compositor->display);
    ww_assert(compositor->backend);
    if (!wlr_multi_backend_add(compositor->backend, compositor->backend_wl)) {
        wlr_log(WLR_ERROR, "failed to add wl backend to multi backend");
        goto fail_backend_multi;
    }
    if (!wlr_multi_backend_add(compositor->backend, compositor->backend_headless)) {
        wlr_log(WLR_ERROR, "failed to add headless backend to multi backend");
        goto fail_backend_multi;
    }

    compositor->renderer = wlr_renderer_autocreate(compositor->backend);
    if (!compositor->renderer) {
        wlr_log(WLR_ERROR, "failed to create wlr_renderer");
        goto fail_renderer;
    }
    wlr_renderer_init_wl_display(compositor->renderer, compositor->display);

    compositor->allocator = wlr_allocator_autocreate(compositor->backend, compositor->renderer);
    if (!compositor->allocator) {
        wlr_log(WLR_ERROR, "failed to create wlr_allocator");
        goto fail_allocator;
    }

    compositor->compositor = wlr_compositor_create(compositor->display, 5, compositor->renderer);
    if (!compositor->compositor) {
        wlr_log(WLR_ERROR, "failed to create wlr_compositor");
        goto fail_compositor;
    }
    ww_assert(wlr_subcompositor_create(compositor->display));
    ww_assert(wlr_data_device_manager_create(compositor->display));

    compositor->dmabuf_export = wlr_export_dmabuf_manager_v1_create(compositor->display);
    if (!compositor->dmabuf_export) {
        wlr_log(WLR_ERROR, "failed to create dmabuf_export_manager");
        goto fail_compositor;
    }

    compositor->output_layout = wlr_output_layout_create();
    ww_assert(compositor->output_layout);
    wl_list_init(&compositor->outputs);
    compositor->on_new_output.notify = on_new_output;
    wl_signal_add(&compositor->backend->events.new_output, &compositor->on_new_output);

    compositor->scene = wlr_scene_create();
    ww_assert(compositor->scene);
    compositor->scene_layout =
        wlr_scene_attach_output_layout(compositor->scene, compositor->output_layout);
    ww_assert(compositor->scene_layout);

    compositor->mouse_sens = 1.0;
    compositor->cursor = wlr_cursor_create();
    ww_assert(compositor->cursor);
    compositor->pointer_constraints = wlr_pointer_constraints_v1_create(compositor->display);
    ww_assert(compositor->pointer_constraints);
    compositor->relative_pointer = wlr_relative_pointer_manager_v1_create(compositor->display);
    ww_assert(compositor->relative_pointer);
    compositor->on_new_constraint.notify = on_new_constraint;
    wl_signal_add(&compositor->pointer_constraints->events.new_constraint,
                  &compositor->on_new_constraint);
    wlr_cursor_attach_output_layout(compositor->cursor, compositor->output_layout);
    compositor->cursor_manager =
        wlr_xcursor_manager_create(compositor->config.cursor_theme, compositor->config.cursor_size);
    ww_assert(compositor->cursor_manager);
    wlr_cursor_set_xcursor(compositor->cursor, compositor->cursor_manager, "default");

    compositor->on_cursor_axis.notify = on_cursor_axis;
    compositor->on_cursor_button.notify = on_cursor_button;
    compositor->on_cursor_frame.notify = on_cursor_frame;
    compositor->on_cursor_motion.notify = on_cursor_motion;
    compositor->on_cursor_motion_absolute.notify = on_cursor_motion_absolute;
    wl_signal_add(&compositor->cursor->events.axis, &compositor->on_cursor_axis);
    wl_signal_add(&compositor->cursor->events.button, &compositor->on_cursor_button);
    wl_signal_add(&compositor->cursor->events.frame, &compositor->on_cursor_frame);
    wl_signal_add(&compositor->cursor->events.motion, &compositor->on_cursor_motion);
    wl_signal_add(&compositor->cursor->events.motion_absolute,
                  &compositor->on_cursor_motion_absolute);

    compositor->seat = wlr_seat_create(compositor->display, "seat0");
    ww_assert(compositor->seat);
    wl_list_init(&compositor->keyboards);
    compositor->on_new_input.notify = on_new_input;
    compositor->on_request_cursor.notify = on_request_cursor;
    compositor->on_request_set_selection.notify = on_request_set_selection;
    wl_signal_add(&compositor->backend->events.new_input, &compositor->on_new_input);
    wl_signal_add(&compositor->seat->events.request_set_cursor, &compositor->on_request_cursor);
    wl_signal_add(&compositor->seat->events.request_set_selection,
                  &compositor->on_request_set_selection);

    compositor->xwayland = wlr_xwayland_create(compositor->display, compositor->compositor, false);
    if (!compositor->xwayland) {
        wlr_log(WLR_ERROR, "failed to create wlr_xwayland");
        goto fail_xwayland;
    }
    wl_list_init(&compositor->windows);
    compositor->on_xwayland_new_surface.notify = on_xwayland_new_surface;
    compositor->on_xwayland_ready.notify = on_xwayland_ready;
    wl_signal_add(&compositor->xwayland->events.new_surface, &compositor->on_xwayland_new_surface);
    wl_signal_add(&compositor->xwayland->events.ready, &compositor->on_xwayland_ready);

    return compositor;

fail_xwayland:
    wlr_xcursor_manager_destroy(compositor->cursor_manager);
    wlr_cursor_destroy(compositor->cursor);
    wlr_scene_node_destroy(&compositor->scene->tree.node);
    wlr_output_layout_destroy(compositor->output_layout);

fail_compositor:
    wlr_allocator_destroy(compositor->allocator);

fail_allocator:
    wlr_renderer_destroy(compositor->renderer);

fail_backend_multi:
fail_renderer:
    zwp_relative_pointer_v1_destroy(compositor->remote_relative_pointer);

fail_registry:
    if (compositor->remote_relative_pointer_manager) {
        zwp_relative_pointer_manager_v1_destroy(compositor->remote_relative_pointer_manager);
    }
    if (compositor->remote_pointer_constraints) {
        zwp_pointer_constraints_v1_destroy(compositor->remote_pointer_constraints);
    }
    if (compositor->remote_pointer) {
        wl_pointer_destroy(compositor->remote_pointer);
    }
    if (compositor->remote_seat) {
        wl_seat_destroy(compositor->remote_seat);
    }
    wlr_backend_destroy(compositor->backend);

fail_backend_wl:
    wlr_backend_destroy(compositor->backend_headless);

fail_backend_headless:
    wl_display_destroy(compositor->display);

fail_display:
    free(compositor);
    return NULL;
}

void
compositor_destroy(struct compositor *compositor) {
    ww_assert(compositor);

    xcb_disconnect(compositor->xcb);
    wl_list_remove(&compositor->on_xwayland_new_surface.link);
    xwm_destroy(compositor->xwayland->xwm);
    wlr_xwayland_destroy(compositor->xwayland);
    wl_display_destroy_clients(compositor->display);
    wlr_xcursor_manager_destroy(compositor->cursor_manager);
    wlr_cursor_destroy(compositor->cursor);

    // We must destroy remote objects before the remote display (from the Wayland backend) is
    // closed.
    zwp_relative_pointer_v1_destroy(compositor->remote_relative_pointer);
    zwp_relative_pointer_manager_v1_destroy(compositor->remote_relative_pointer_manager);
    zwp_pointer_constraints_v1_destroy(compositor->remote_pointer_constraints);
    wl_pointer_destroy(compositor->remote_pointer);
    wl_seat_destroy(compositor->remote_seat);

    wlr_backend_destroy(compositor->backend);
    wlr_renderer_destroy(compositor->renderer);
    wlr_allocator_destroy(compositor->allocator);
    wlr_scene_node_destroy(&compositor->scene->tree.node);
    wlr_output_layout_destroy(compositor->output_layout);

    wl_display_destroy(compositor->display);
    free(compositor);
}

struct wl_event_loop *
compositor_get_loop(struct compositor *compositor) {
    ww_assert(compositor);

    return wl_display_get_event_loop(compositor->display);
}

bool
compositor_run(struct compositor *compositor, int display_file_fd) {
    ww_assert(compositor);

    if (!wlr_backend_start(compositor->backend)) {
        wlr_backend_destroy(compositor->backend);
        return false;
    }

    const char *socket = wl_display_add_socket_auto(compositor->display);
    if (!socket) {
        wlr_backend_destroy(compositor->backend);
        return false;
    }
    setenv("WAYLAND_DISPLAY", socket, true);
    setenv("DISPLAY", compositor->xwayland->display_name, true);
    char buf[256];
    ssize_t len =
        snprintf(buf, ARRAY_LEN(buf), "%s\n%s", socket, compositor->xwayland->display_name);
    if (len >= (ssize_t)ARRAY_LEN(buf) || len < 0) {
        wlr_log(WLR_ERROR, "failed to write waywall-display file (%zd)", len);
        return false;
    }
    if (write(display_file_fd, buf, len) == -1) {
        wlr_log_errno(WLR_ERROR, "failed to write waywall-display");
        return false;
    }
    if (ftruncate(display_file_fd, len) == -1) {
        wlr_log_errno(WLR_ERROR, "failed to truncate waywall-display");
        return false;
    }

    wl_display_run(compositor->display);
    return true;
}

void
compositor_stop(struct compositor *compositor) {
    if (compositor->should_stop) {
        wlr_log(WLR_INFO, "received 2nd stop call - terminating");
        wl_display_terminate(compositor->display);
        return;
    }
    compositor->should_stop = true;
    if (wl_list_length(&compositor->windows) == 0) {
        wl_display_terminate(compositor->display);
        return;
    }

    struct window *window;
    wl_list_for_each (window, &compositor->windows, link) {
        wlr_xwayland_surface_close(window->surface);
    }
}

void
compositor_click(struct window *window) {
    // HACK: We send enter and leave notify events to get GLFW to update the cursor position.

    xcb_enter_notify_event_t event = {
        .response_type = XCB_ENTER_NOTIFY,
        .root = window->surface->window_id,
        .event = window->surface->window_id,
        .child = window->surface->window_id,
        0,
    };
    send_event(window->compositor->xcb, window->surface->window_id,
               XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW, (char *)&event);
    event.response_type = XCB_LEAVE_NOTIFY;
    send_event(window->compositor->xcb, window->surface->window_id,
               XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW, (char *)&event);

    xcb_button_press_event_t event2 = {
        .response_type = XCB_BUTTON_PRESS,
        .detail = XCB_BUTTON_INDEX_1,
        .root = window->surface->window_id,
        .event = window->surface->window_id,
        .child = window->surface->window_id,
        0,
    };
    send_event(window->compositor->xcb, window->surface->window_id,
               XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE, (char *)&event2);
    event2.response_type = XCB_BUTTON_RELEASE;
    send_event(window->compositor->xcb, window->surface->window_id,
               XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE, (char *)&event2);
}

int
compositor_get_windows(struct compositor *compositor, struct window ***windows) {
    ww_assert(compositor);
    ww_assert(windows);

    int count = wl_list_length(&compositor->windows);
    if (count == 0) {
        return 0;
    }
    struct window **data = calloc(count, sizeof(struct window *));
    ww_assert(data);
    *windows = data;

    struct window *window;
    int i = 0;
    wl_list_for_each (window, &compositor->windows, link) {
        data[i++] = window;
    };
    return count;
}

void
compositor_load_config(struct compositor *compositor, struct compositor_config config) {
    ww_assert(compositor);

    struct keyboard *keyboard;
    wl_list_for_each (keyboard, &compositor->keyboards, link) {
        wlr_keyboard_set_repeat_info(keyboard->wlr_keyboard, config.repeat_rate,
                                     config.repeat_delay);
    }
    if (config.confine_pointer && !compositor->remote_confined_pointer) {
        if (!compositor->active_constraint) {
            ww_assert(!compositor->remote_locked_pointer);

            compositor->remote_confined_pointer = zwp_pointer_constraints_v1_confine_pointer(
                compositor->remote_pointer_constraints, compositor->wl_output->remote_surface,
                compositor->remote_pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
            ww_assert(compositor->remote_confined_pointer);
        }
    } else if (!config.confine_pointer && compositor->remote_confined_pointer) {
        zwp_confined_pointer_v1_destroy(compositor->remote_confined_pointer);
        compositor->remote_confined_pointer = NULL;
    }

    ww_assert(compositor->background);
    wlr_scene_rect_set_color(compositor->background, (const float *)&config.background_color);
    compositor->config = config;
}

bool
compositor_recreate_output(struct compositor *compositor) {
    if (compositor->wl_output) {
        return false;
    }
    wlr_wl_output_create(compositor->backend_wl);
    return true;
}

void
compositor_send_keys(struct window *window, const struct compositor_key *keys, int count) {
    ww_assert(window);

    for (int i = 0; i < count; i++) {
        xcb_key_press_event_t event = {
            .response_type = keys[i].state ? XCB_KEY_PRESS : XCB_KEY_RELEASE,
            .time = now_msec(),
            .detail = keys[i].keycode + 8, // libinput keycode -> xkb keycode
            .root = window->surface->window_id,
            .event = window->surface->window_id,
            .child = window->surface->window_id,
            .same_screen = true,
            0,
        };
        send_event(window->compositor->xcb, window->surface->window_id,
                   XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE, (char *)&event);
    }
}

void
compositor_set_mouse_sensitivity(struct compositor *compositor, double multiplier) {
    compositor->mouse_sens = multiplier;
}

void
compositor_window_configure(struct window *window, int16_t w, int16_t h) {
    ww_assert(window);

    wlr_xwayland_surface_configure(window->surface, 0, 0, w, h);
}

void
compositor_window_destroy_headless_views(struct window *window) {
    for (int i = 0; i < window->headless_view_count; i++) {
        wlr_scene_node_destroy(&window->headless_views[i].tree->node);
    }
    window->headless_view_count = 0;
}

void
compositor_window_focus(struct compositor *compositor, struct window *window) {
    ww_assert(compositor);

    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(compositor->seat);
    wlr_cursor_set_xcursor(compositor->cursor, compositor->cursor_manager, "default");

    if (window) {
        wlr_xwayland_set_seat(compositor->xwayland, compositor->seat);
        wlr_xwayland_surface_activate(window->surface, true);
        wlr_scene_node_raise_to_top(&window->scene_tree->node);

        if (keyboard) {
            wlr_seat_keyboard_notify_enter(compositor->seat, window->surface->surface,
                                           keyboard->keycodes, keyboard->num_keycodes,
                                           &keyboard->modifiers);
        }
        double x, y;
        global_to_surface(compositor, &window->scene_tree->node, compositor->cursor->x,
                          compositor->cursor->y, &x, &y);
        wlr_seat_pointer_notify_enter(compositor->seat, window->surface->surface, x, y);

        struct wlr_pointer_constraint_v1 *constraint =
            wlr_pointer_constraints_v1_constraint_for_surface(
                compositor->pointer_constraints, window->surface->surface, compositor->seat);
        handle_constraint(compositor, constraint);
    } else {
        if (!compositor->focused_window) {
            return;
        }
        handle_constraint(compositor, NULL);
        wlr_xwayland_surface_activate(compositor->focused_window->surface, false);
        wlr_seat_keyboard_notify_clear_focus(compositor->seat);
        wlr_seat_pointer_notify_clear_focus(compositor->seat);
    }
    compositor->focused_window = window;
}

pid_t
compositor_window_get_pid(struct window *window) {
    ww_assert(window);

    return window->surface->pid > 0 ? window->surface->pid : -1;
}

struct headless_view *
compositor_window_make_headless_view(struct window *window) {
    ww_assert(window);
    ww_assert(window->headless_view_count < (int)ARRAY_LEN(window->headless_views));

    struct headless_view *view = &window->headless_views[window->headless_view_count++];
    view->tree = wlr_scene_tree_create(&window->compositor->scene->tree);
    ww_assert(view->tree);
    wlr_scene_node_set_enabled(&view->tree->node, true);
    view->surface = wlr_scene_surface_create(view->tree, window->surface->surface);
    ww_assert(view->surface);
    wlr_scene_node_set_position(&view->tree->node, HEADLESS_X, HEADLESS_Y);

    return view;
}

void
compositor_window_set_dest(struct window *window, struct wlr_box box) {
    wlr_scene_node_set_position(&window->scene_tree->node, WL_X + box.x, WL_Y + box.y);
    wlr_scene_buffer_set_dest_size(window->scene_surface->buffer, box.width, box.height);
}

void
compositor_hview_set_dest(struct headless_view *view, struct wlr_box box) {
    wlr_scene_node_set_position(&view->tree->node, HEADLESS_X + box.x, HEADLESS_Y + box.y);
    wlr_scene_buffer_set_dest_size(view->surface->buffer, box.width, box.height);
}

void
compositor_hview_set_src(struct headless_view *view, struct wlr_box box) {
    // TODO: This can trigger an assert in wlroots:render/pass.c. Should investigate for stability.
    const struct wlr_fbox fbox = {
        .x = box.x,
        .y = box.y,
        .width = box.width,
        .height = box.height,
    };
    wlr_scene_buffer_set_source_box(view->surface->buffer, &fbox);
}

void
compositor_hview_set_top(struct headless_view *view) {
    wlr_scene_node_raise_to_top(&view->tree->node);
}

void
compositor_rect_configure(struct wlr_scene_rect *rect, struct wlr_box box) {
    wlr_scene_node_set_position(&rect->node, WL_X + box.x, WL_Y + box.y);
    wlr_scene_rect_set_size(rect, box.width, box.height);
}

struct wlr_scene_rect *
compositor_rect_create(struct compositor *compositor, struct wlr_box box, float color[4]) {
    struct wlr_scene_rect *rect =
        wlr_scene_rect_create(&compositor->scene->tree, box.width, box.height, color);
    wlr_scene_node_set_position(&rect->node, WL_X + box.x, WL_Y + box.y);
    ww_assert(rect);
    return rect;
}

void
compositor_rect_set_color(struct wlr_scene_rect *rect, float color[4]) {
    wlr_scene_rect_set_color(rect, color);
}

void
compositor_rect_toggle(struct wlr_scene_rect *rect, bool state) {
    if (state) {
        wlr_scene_node_set_enabled(&rect->node, true);
        wlr_scene_node_raise_to_top(&rect->node);
    } else {
        wlr_scene_node_set_enabled(&rect->node, false);
    }
}
