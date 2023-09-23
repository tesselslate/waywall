// TODO: pointer constraints
// TODO: get wl backend

#include "compositor.h"
#include "pointer-constraints-unstable-v1-protocol.h"
#include "util.h"
#include "xdg-shell-protocol.h"
#include <stdlib.h>
#include <time.h>
#include <wayland-client.h>
#include <wlr/backend/wayland.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include <xcb/xproto.h>
#include <xkbcommon/xkbcommon.h>

struct compositor {
    struct wl_display *display;
    struct wlr_allocator *allocator;
    struct wlr_backend *backend;
    struct wlr_compositor *compositor;
    struct wlr_renderer *renderer;
    struct wlr_scene *scene;
    struct wlr_scene_output_layout *scene_layout;

    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_manager;
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

    struct wlr_xdg_shell *xdg_shell;
    struct wl_list views;
    struct wl_listener on_new_xdg_surface;

    struct wlr_xwayland *xwayland;
    struct wl_list windows;
    struct wl_listener on_xwayland_new_surface;
    struct wl_listener on_xwayland_ready;

    struct wl_display *remote_display;
    struct wl_pointer *remote_pointer;
    struct wl_seat *remote_seat;
    struct zwp_pointer_constraints_v1 *remote_pointer_constraints;

    struct compositor_vtable vtable;
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
    struct wl_surface *remote_surface;

    struct wl_listener on_frame;
    struct wl_listener on_request_state;
    struct wl_listener on_destroy;
};

struct view {
    struct wl_list link;

    struct compositor *compositor;
    struct wlr_xdg_toplevel *xdg_toplevel;
    struct wlr_scene_tree *scene_tree;

    struct wl_listener on_map;
    struct wl_listener on_unmap;
    struct wl_listener on_destroy;
    struct wl_listener on_request_move;
    struct wl_listener on_request_resize;
    struct wl_listener on_request_maximize;
    struct wl_listener on_request_fullscreen;
};

struct window {
    struct wl_list link;

    struct compositor *compositor;
    struct wlr_xwayland_surface *surface;

    struct wl_listener on_destroy;
    struct wl_listener on_request_activate;
    struct wl_listener on_request_configure;
    struct wl_listener on_request_fullscreen;
};

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
    }
}

static void
on_registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    // TODO
}

static struct wl_registry_listener registry_listener = {
    .global = on_registry_global,
    .global_remove = on_registry_global_remove,
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
    wlr_cursor_warp_absolute(compositor->cursor, &wlr_event->pointer->base, wlr_event->x,
                             wlr_event->y);
    struct compositor_motion_event event = {
        .x = wlr_event->x,
        .y = wlr_event->y,
        .time_msec = wlr_event->time_msec,
    };
    compositor->vtable.motion(event);
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
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, keycode, &syms);
    uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
    struct compositor_key_event event = {
        .syms = syms,
        .nsyms = nsyms,
        .modifiers = modifiers,
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
}

static void
on_output_destroy(struct wl_listener *listener, void *data) {
    struct output *output = wl_container_of(listener, output, on_destroy);
    wl_list_remove(&output->on_destroy.link);
    wl_list_remove(&output->on_frame.link);
    wl_list_remove(&output->on_request_state.link);
    wl_list_remove(&output->link);

    if (wl_list_length(&output->compositor->outputs) == 0) {
        // TODO: change compositor kill logic
        wlr_log(WLR_INFO, "last output destroyed - stopping");
        wl_display_terminate(output->compositor->display);
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

    // No modesetting is necessary since we only support Wayland.
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    struct output *output = calloc(1, sizeof(struct output));
    ww_assert(output);
    output->compositor = compositor;
    output->wlr_output = wlr_output;
    output->remote_surface = wlr_wl_output_get_surface(wlr_output);

    output->on_frame.notify = on_output_frame;
    output->on_request_state.notify = on_output_request_state;
    output->on_destroy.notify = on_output_destroy;
    wl_signal_add(&wlr_output->events.frame, &output->on_frame);
    wl_signal_add(&wlr_output->events.request_state, &output->on_request_state);
    wl_signal_add(&wlr_output->events.destroy, &output->on_destroy);
    wl_list_insert(&compositor->outputs, &output->link);

    struct wlr_output_layout_output *layout_output =
        wlr_output_layout_add_auto(compositor->output_layout, wlr_output);
    struct wlr_scene_output *scene_output = wlr_scene_output_create(compositor->scene, wlr_output);
    wlr_scene_output_layout_add_output(compositor->scene_layout, layout_output, scene_output);
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
    // TODO: Allow configuring repeat rate
    wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

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
on_window_destroy(struct wl_listener *listener, void *data) {
    struct window *window = wl_container_of(listener, window, on_destroy);
    wl_list_remove(&window->on_destroy.link);
    wl_list_remove(&window->on_request_activate.link);
    wl_list_remove(&window->on_request_configure.link);
    wl_list_remove(&window->on_request_fullscreen.link);
    wl_list_remove(&window->link);
    free(window);
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

    // TODO: Update this logic to do what it's supposed to (for now just accept whatever)
    struct wlr_xwayland_surface_configure_event *event = data;
    wlr_xwayland_surface_configure(window->surface, event->x, event->y, event->width,
                                   event->height);
}

static void
on_window_request_fullscreen(struct wl_listener *listener, void *data) {
    struct window *window = wl_container_of(listener, window, on_request_fullscreen);
    wlr_log(WLR_DEBUG, "window %d requested fullscreen", window->surface->window_id);
}

static void
on_xdg_toplevel_map(struct wl_listener *listener, void *data) {
    struct view *view = wl_container_of(listener, view, on_map);
    wl_list_insert(&view->compositor->views, &view->link);
}

static void
on_xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
    struct view *view = wl_container_of(listener, view, on_unmap);
    wl_list_remove(&view->link);
}

static void
on_xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
    struct view *view = wl_container_of(listener, view, on_destroy);
    wl_list_remove(&view->on_map.link);
    wl_list_remove(&view->on_unmap.link);
    wl_list_remove(&view->on_destroy.link);
    wl_list_remove(&view->on_request_move.link);
    wl_list_remove(&view->on_request_resize.link);
    wl_list_remove(&view->on_request_maximize.link);
    wl_list_remove(&view->on_request_fullscreen.link);
    free(view);
}

static void
on_xdg_toplevel_request_move(struct wl_listener *listener, void *data) {
    // Do nothing. We do not want interactive window moving.
}

static void
on_xdg_toplevel_request_resize(struct wl_listener *listener, void *data) {
    // Do nothing. We do not want interactive window resizing.
}

static void
on_xdg_toplevel_request_maximize(struct wl_listener *listener, void *data) {
    struct view *view = wl_container_of(listener, view, on_request_maximize);
    // Windows cannot maximize themselves but a response must be sent.
    wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
}

static void
on_xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
    struct view *view = wl_container_of(listener, view, on_request_fullscreen);
    // Windows cannot fullscreen themselves but a response must be sent.
    wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
}

static void
on_xdg_new_surface(struct wl_listener *listener, void *data) {
    struct compositor *compositor = wl_container_of(listener, compositor, on_new_xdg_surface);
    struct wlr_xdg_surface *xdg_surface = data;

    if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
        wlr_log(WLR_DEBUG, "ignoring non-toplevel xdg surface");
        return;
    }
    struct view *view = calloc(1, sizeof(struct view));
    ww_assert(view);
    view->compositor = compositor;
    view->xdg_toplevel = xdg_surface->toplevel;
    view->scene_tree =
        wlr_scene_xdg_surface_create(&compositor->scene->tree, view->xdg_toplevel->base);
    view->scene_tree->node.data = view;
    xdg_surface->data = view->scene_tree;
    view->on_map.notify = on_xdg_toplevel_map;
    view->on_unmap.notify = on_xdg_toplevel_unmap;
    view->on_destroy.notify = on_xdg_toplevel_destroy;
    view->on_request_move.notify = on_xdg_toplevel_request_move;
    view->on_request_resize.notify = on_xdg_toplevel_request_resize;
    view->on_request_maximize.notify = on_xdg_toplevel_request_maximize;
    view->on_request_fullscreen.notify = on_xdg_toplevel_request_fullscreen;
    wl_signal_add(&xdg_surface->surface->events.map, &view->on_map);
    wl_signal_add(&xdg_surface->surface->events.unmap, &view->on_unmap);
    wl_signal_add(&xdg_surface->surface->events.destroy, &view->on_destroy);
    wl_signal_add(&view->xdg_toplevel->events.request_move, &view->on_request_move);
    wl_signal_add(&view->xdg_toplevel->events.request_resize, &view->on_request_resize);
    wl_signal_add(&view->xdg_toplevel->events.request_maximize, &view->on_request_maximize);
    wl_signal_add(&view->xdg_toplevel->events.request_fullscreen, &view->on_request_fullscreen);
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
    surface->data = window;
    wl_list_insert(&compositor->windows, &window->link);
    window->surface = surface;
    window->on_destroy.notify = on_window_destroy;
    window->on_request_activate.notify = on_window_request_activate;
    window->on_request_configure.notify = on_window_request_configure;
    window->on_request_fullscreen.notify = on_window_request_fullscreen;
    wl_signal_add(&surface->events.destroy, &window->on_destroy);
    wl_signal_add(&surface->events.request_activate, &window->on_request_activate);
    wl_signal_add(&surface->events.request_configure, &window->on_request_configure);
    wl_signal_add(&surface->events.request_fullscreen, &window->on_request_fullscreen);
}

static void
on_xwayland_ready(struct wl_listener *listener, void *data) {}

struct compositor *
compositor_create(struct compositor_vtable vtable) {
    // TODO: proper teardown on failure
    struct compositor *compositor = calloc(1, sizeof(struct compositor));
    if (!compositor) {
        wlr_log(WLR_ERROR, "failed to allocate compositor");
        return NULL;
    }

    ww_assert(vtable.button);
    ww_assert(vtable.key);
    ww_assert(vtable.motion);
    compositor->vtable = vtable;

    compositor->display = wl_display_create();
    if (!compositor->display) {
        wlr_log(WLR_ERROR, "failed to create wl_display");
        return NULL;
    }

    compositor->backend = wlr_wl_backend_create(compositor->display, NULL);
    if (!compositor->backend) {
        wlr_log(WLR_ERROR, "failed to create wlr_backend");
        return NULL;
    }
    compositor->remote_display = wlr_wl_backend_get_remote_display(compositor->backend);
    ww_assert(compositor->remote_display);
    struct wl_registry *remote_registry = wl_display_get_registry(compositor->remote_display);
    wl_registry_add_listener(remote_registry, &registry_listener, compositor);
    wl_display_roundtrip(compositor->remote_display);
    if (!compositor->remote_pointer || !compositor->remote_pointer_constraints) {
        wlr_log(WLR_ERROR, "failed to acquire objects for pointer constraints");
        return NULL;
    }
    wlr_wl_output_create(compositor->backend);

    compositor->renderer = wlr_renderer_autocreate(compositor->backend);
    if (!compositor->renderer) {
        wlr_log(WLR_ERROR, "failed to create wlr_renderer");
        return NULL;
    }
    wlr_renderer_init_wl_display(compositor->renderer, compositor->display);

    compositor->allocator = wlr_allocator_autocreate(compositor->backend, compositor->renderer);
    if (!compositor->allocator) {
        wlr_log(WLR_ERROR, "failed to create wlr_allocator");
        return NULL;
    }

    compositor->compositor = wlr_compositor_create(compositor->display, 5, compositor->renderer);
    if (!compositor->compositor) {
        wlr_log(WLR_ERROR, "failed to create wlr_compositor");
        return NULL;
    }
    ww_assert(wlr_subcompositor_create(compositor->display));
    ww_assert(wlr_data_device_manager_create(compositor->display));

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

    compositor->cursor = wlr_cursor_create();
    ww_assert(compositor->cursor);
    wlr_cursor_attach_output_layout(compositor->cursor, compositor->output_layout);
    // TODO: Allow configuring cursor theme and size
    compositor->cursor_manager = wlr_xcursor_manager_create(NULL, 24);
    ww_assert(compositor->cursor_manager);

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

    compositor->xdg_shell = wlr_xdg_shell_create(compositor->display, 6);
    if (!compositor->xdg_shell) {
        wlr_log(WLR_ERROR, "failed to create wlr_xdg_shell");
        wlr_backend_destroy(compositor->backend);
        return NULL;
    }
    compositor->on_new_xdg_surface.notify = on_xdg_new_surface;
    wl_signal_add(&compositor->xdg_shell->events.new_surface, &compositor->on_new_xdg_surface);

    compositor->xwayland = wlr_xwayland_create(compositor->display, compositor->compositor, false);
    if (!compositor->xwayland) {
        wlr_log(WLR_ERROR, "failed to create wlr_xwayland");
        wlr_backend_destroy(compositor->backend);
        return NULL;
    }
    wl_list_init(&compositor->windows);
    compositor->on_xwayland_new_surface.notify = on_xwayland_new_surface;
    compositor->on_xwayland_ready.notify = on_xwayland_ready;
    wl_signal_add(&compositor->xwayland->events.new_surface, &compositor->on_xwayland_new_surface);
    wl_signal_add(&compositor->xwayland->events.ready, &compositor->on_xwayland_ready);

    return compositor;
}

struct wl_event_loop *
compositor_get_loop(struct compositor *compositor) {
    ww_assert(compositor);

    return wl_display_get_event_loop(compositor->display);
}

void
compositor_destroy(struct compositor *compositor) {
    ww_assert(compositor);

    if (compositor->xwayland != NULL) {
        wlr_xwayland_destroy(compositor->xwayland);
    }
    if (compositor->display != NULL) {
        wl_display_destroy_clients(compositor->display);
        wl_display_destroy(compositor->display);
    }
    free(compositor);
}

bool
compositor_run(struct compositor *compositor) {
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

    wl_display_run(compositor->display);
    return true;
}
