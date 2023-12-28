#include "compositor/wl_seat.h"
#include "compositor/server.h"
#include "compositor/wl_compositor.h"
#include "util.h"
#include <wayland-client.h>
#include <wayland-server.h>

// TODO: is sending out of bounds pointer events bad?

/*
 *  Most keyboard and mouse events are implemented. Touch functionality is not implemented because
 *  GLFW does not make use of it.
 *
 *  The goal here is to pass through input events from the host compositor to whatever surface has
 *  "input focus."
 *    - We do not want to pass through `enter` and `leave` events. The surfaces will receive them
 *      when the "input focus" changes.
 *    - We want to allow users to customize their keymap, repeat info, and cursor theme.
 */

#define VERSION 8

static inline bool
has_focus(struct server_seat *seat, struct wl_resource *resource) {
    if (!seat->input_focus) {
        return false;
    }

    struct server_surface *surface = server_view_get_surface(seat->input_focus);
    return wl_resource_get_client(surface->resource) == wl_resource_get_client(resource);
}

static void
send_pointer_enter(struct server_seat *seat, struct wl_resource *pointer_resource) {
    ww_assert(has_focus(seat, pointer_resource));

    struct server_surface *surface = server_view_get_surface(seat->input_focus);
    double x = seat->remote.pointer_x - seat->input_focus->bounds.x;
    double y = seat->remote.pointer_y - seat->input_focus->bounds.y;

    wl_pointer_send_enter(pointer_resource, next_serial(pointer_resource), surface->resource,
                          wl_fixed_from_double(x), wl_fixed_from_double(y));
}

static void
send_pointer_motion(struct server_seat *seat) {
    struct wl_resource *pointer_resource;

    double x = seat->remote.pointer_x - seat->input_focus->bounds.x;
    double y = seat->remote.pointer_y - seat->input_focus->bounds.y;
    uint32_t time = current_time();

    wl_resource_for_each(pointer_resource, &seat->pointers) {
        if (!has_focus(seat, pointer_resource)) {
            continue;
        }

        wl_pointer_send_motion(pointer_resource, time, wl_fixed_from_double(x),
                               wl_fixed_from_double(y));
    }
}

static void
send_keyboard_enter(struct server_seat *seat, struct wl_resource *keyboard_resource) {
    ww_assert(has_focus(seat, keyboard_resource));

    struct server_surface *surface = server_view_get_surface(seat->input_focus);

    struct wl_array keys;
    wl_array_init(&keys);
    for (size_t i = 0; i < seat->remote.keys.count; i++) {
        uint32_t *key = wl_array_add(&keys, sizeof(uint32_t));
        *key = seat->remote.keys.data[i];
    }

    wl_keyboard_send_enter(keyboard_resource, next_serial(keyboard_resource), surface->resource,
                           &keys);
    wl_array_release(&keys);

    wl_keyboard_send_modifiers(keyboard_resource, next_serial(keyboard_resource),
                               seat->remote.mods_depressed, seat->remote.mods_latched,
                               seat->remote.mods_locked, seat->remote.group);
}

static void
on_view_destroy(struct wl_listener *listener, void *data) {
    struct server_seat *seat = wl_container_of(listener, seat, on_destroy);

    server_seat_set_input_focus(seat, NULL);
}

static void
on_pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                 struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    struct server_seat *seat = data;

    seat->remote.last_pointer_enter = serial;
    seat->remote.pointer_x = wl_fixed_to_double(x);
    seat->remote.pointer_y = wl_fixed_to_double(y);

    if (seat->input_focus) {
        send_pointer_motion(seat);
    }
}

static void
on_pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                 struct wl_surface *surface) {
    struct server_seat *seat = data;

    if (!seat->input_focus || seat->remote.buttons.count == 0) {
        return;
    }

    struct wl_resource *pointer_resource;
    uint32_t time = current_time();

    wl_resource_for_each(pointer_resource, &seat->pointers) {
        if (!has_focus(seat, pointer_resource)) {
            continue;
        }

        for (size_t i = 0; i < seat->remote.buttons.count; i++) {
            wl_pointer_send_button(pointer_resource, next_serial(pointer_resource), time,
                                   seat->remote.buttons.data[i], WL_POINTER_BUTTON_STATE_RELEASED);
        }
    }

    seat->remote.buttons.count = 0;
}

static void
on_pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x,
                  wl_fixed_t y) {
    struct server_seat *seat = data;

    seat->remote.pointer_x = wl_fixed_to_double(x);
    seat->remote.pointer_y = wl_fixed_to_double(y);

    if (seat->input_focus) {
        send_pointer_motion(seat);
    }
}

static void
on_pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time,
                  uint32_t button, uint32_t state) {
    struct server_seat *seat = data;

    if (seat->input_focus) {
        struct wl_resource *pointer_resource;
        time = current_time();

        wl_resource_for_each(pointer_resource, &seat->pointers) {
            if (!has_focus(seat, pointer_resource)) {
                continue;
            }

            wl_pointer_send_button(pointer_resource, next_serial(pointer_resource), time, button,
                                   state);
        }
    }

    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        u32_array_push(&seat->remote.buttons, button);
    } else {
        for (size_t i = 0; i < seat->remote.buttons.count; i++) {
            if (seat->remote.buttons.data[i] == button) {
                u32_array_remove(&seat->remote.buttons, i);
                return;
            }
        }
        LOG(LOG_ERROR, "received mismatched button release for button %" PRIu32, button);
    }
}

static void
on_pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis,
                wl_fixed_t value) {
    struct server_seat *seat = data;

    if (!seat->input_focus) {
        return;
    }

    struct wl_resource *pointer_resource;
    time = current_time();

    wl_resource_for_each(pointer_resource, &seat->pointers) {
        if (!has_focus(seat, pointer_resource)) {
            continue;
        }

        wl_pointer_send_axis(pointer_resource, time, axis, value);
    }
}

static void
on_pointer_frame(void *data, struct wl_pointer *pointer) {
    struct server_seat *seat = data;

    if (!seat->input_focus) {
        return;
    }

    struct wl_resource *pointer_resource;

    wl_resource_for_each(pointer_resource, &seat->pointers) {
        if (!has_focus(seat, pointer_resource)) {
            continue;
        }

        if (wl_resource_get_version(pointer_resource) >= WL_POINTER_FRAME_SINCE_VERSION) {
            wl_pointer_send_frame(pointer_resource);
        }
    }
}

static void
on_pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t axis_source) {
    struct server_seat *seat = data;

    if (!seat->input_focus) {
        return;
    }

    struct wl_resource *pointer_resource;

    wl_resource_for_each(pointer_resource, &seat->pointers) {
        if (!has_focus(seat, pointer_resource)) {
            continue;
        }

        if (wl_resource_get_version(pointer_resource) >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION) {
            wl_pointer_send_axis_source(pointer_resource, axis_source);
        }
    }
}

static void
on_pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis) {
    struct server_seat *seat = data;

    if (!seat->input_focus) {
        return;
    }

    struct wl_resource *pointer_resource;
    time = current_time();

    wl_resource_for_each(pointer_resource, &seat->pointers) {
        if (!has_focus(seat, pointer_resource)) {
            continue;
        }

        if (wl_resource_get_version(pointer_resource) >= WL_POINTER_AXIS_STOP_SINCE_VERSION) {
            wl_pointer_send_axis_stop(pointer_resource, time, axis);
        }
    }
}

static void
on_pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete) {
    struct server_seat *seat = data;

    if (!seat->input_focus) {
        return;
    }

    struct wl_resource *pointer_resource;

    wl_resource_for_each(pointer_resource, &seat->pointers) {
        if (!has_focus(seat, pointer_resource)) {
            continue;
        }

        uint32_t version = wl_resource_get_version(pointer_resource);
        if (version >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION &&
            version < WL_POINTER_AXIS_VALUE120_SINCE_VERSION) {
            wl_pointer_send_axis_discrete(pointer_resource, axis, discrete);
        }
    }
}

static void
on_pointer_axis_value120(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t value120) {
    struct server_seat *seat = data;

    if (!seat->input_focus) {
        return;
    }

    struct wl_resource *pointer_resource;

    wl_resource_for_each(pointer_resource, &seat->pointers) {
        if (!has_focus(seat, pointer_resource)) {
            continue;
        }

        if (wl_resource_get_version(pointer_resource) >= WL_POINTER_AXIS_VALUE120_SINCE_VERSION) {
            wl_pointer_send_axis_value120(pointer_resource, axis, value120);
        }
    }
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = on_pointer_enter,
    .leave = on_pointer_leave,
    .motion = on_pointer_motion,
    .button = on_pointer_button,
    .axis = on_pointer_axis,
    .frame = on_pointer_frame,
    .axis_source = on_pointer_axis_source,
    .axis_stop = on_pointer_axis_stop,
    .axis_discrete = on_pointer_axis_discrete,
    .axis_value120 = on_pointer_axis_value120,
};

static void
on_keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd,
                   uint32_t size) {
    struct server_seat *seat = data;

    if (seat->remote.keymap_fd >= 0) {
        close(seat->remote.keymap_fd);
    }

    seat->remote.keymap_format = format;
    seat->remote.keymap_fd = fd;
    seat->remote.keymap_size = size;

    // TODO: send keymap info
}

static void
on_keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                  struct wl_surface *surface, struct wl_array *keys) {
    struct server_seat *seat = data;

    seat->remote.keys.count = 0;

    uint32_t *key;
    wl_array_for_each(key, keys) {
        u32_array_push(&seat->remote.keys, *key);
    }
}

static void
on_keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                  struct wl_surface *surface) {
    struct server_seat *seat = data;

    if (seat->input_focus) {
        struct wl_resource *keyboard_resource;
        uint32_t time = current_time();

        wl_resource_for_each(keyboard_resource, &seat->keyboards) {
            if (!has_focus(seat, keyboard_resource)) {
                continue;
            }

            for (size_t i = 0; i < seat->remote.keys.count; i++) {
                wl_keyboard_send_key(keyboard_resource, next_serial(keyboard_resource), time,
                                     seat->remote.keys.data[i], WL_KEYBOARD_KEY_STATE_RELEASED);
            }
        }
    }

    seat->remote.keys.count = 0;
}

static void
on_keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time,
                uint32_t key, uint32_t state) {
    struct server_seat *seat = data;

    if (seat->input_focus) {
        struct wl_resource *keyboard_resource;
        time = current_time();

        wl_resource_for_each(keyboard_resource, &seat->keyboards) {
            if (!has_focus(seat, keyboard_resource)) {
                continue;
            }

            wl_keyboard_send_key(keyboard_resource, next_serial(keyboard_resource), time, key,
                                 state);
        }
    }

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        u32_array_push(&seat->remote.keys, key);
    } else {
        for (size_t i = 0; i < seat->remote.keys.count; i++) {
            if (seat->remote.keys.data[i] == key) {
                u32_array_remove(&seat->remote.keys, i);
                return;
            }
        }
        LOG(LOG_ERROR, "received mismatch key release for key %" PRIu32, key);
    }
}

static void
on_keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                      uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked,
                      uint32_t group) {
    struct server_seat *seat = data;

    seat->remote.mods_depressed = mods_depressed;
    seat->remote.mods_latched = mods_latched;
    seat->remote.mods_locked = mods_locked;
    seat->remote.group = group;

    if (!seat->input_focus) {
        return;
    }

    struct wl_resource *keyboard_resource;

    wl_resource_for_each(keyboard_resource, &seat->keyboards) {
        if (!has_focus(seat, keyboard_resource)) {
            continue;
        }

        wl_keyboard_send_modifiers(keyboard_resource, next_serial(keyboard_resource),
                                   mods_depressed, mods_latched, mods_locked, group);
    }
}

static void
on_keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay) {
    // TODO: use if the user does not manually specify their own repeat info?
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = on_keyboard_keymap,
    .enter = on_keyboard_enter,
    .leave = on_keyboard_leave,
    .key = on_keyboard_key,
    .modifiers = on_keyboard_modifiers,
    .repeat_info = on_keyboard_repeat_info,
};

static void
on_seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities) {
    struct server_seat *seat = data;

    bool has_pointer = (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0;
    bool has_keyboard = (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0;

    // TODO: I think there is a weird edge case here where if a capability gets removed while the
    // user is performing an input with it (e.g. holding a key or button) then it gets stuck. This
    // is really niche though, I don't think wlroots revokes capabilities? Maybe other compositors
    // do.

    if (has_pointer && !seat->remote.pointer) {
        seat->remote.pointer = wl_seat_get_pointer(wl_seat);
        wl_pointer_add_listener(seat->remote.pointer, &pointer_listener, seat);
    } else if (!has_pointer && seat->remote.pointer) {
        wl_pointer_release(seat->remote.pointer);
        seat->remote.pointer = NULL;
    }

    if (has_keyboard && !seat->remote.keyboard) {
        seat->remote.keyboard = wl_seat_get_keyboard(wl_seat);
        wl_keyboard_add_listener(seat->remote.keyboard, &keyboard_listener, seat);
    } else if (!has_keyboard && seat->remote.keyboard) {
        wl_keyboard_release(seat->remote.keyboard);
        seat->remote.keyboard = NULL;
    }
}

static void
on_seat_name(void *data, struct wl_seat *wl_seat, const char *name) {
    // Unused.
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = on_seat_capabilities,
    .name = on_seat_name,
};

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_seat *seat = wl_container_of(listener, seat, display_destroy);

    if (seat->remote.keyboard) {
        wl_keyboard_release(seat->remote.keyboard);
    }
    if (seat->remote.pointer) {
        wl_pointer_release(seat->remote.pointer);
    }
    if (seat->remote.seat) {
        wl_seat_release(seat->remote.seat);
    }

    wl_global_destroy(seat->global);

    free(seat);
}

static void
handle_pointer_set_cursor(struct wl_client *client, struct wl_resource *resource, uint32_t serial,
                          struct wl_resource *surface_resource, int32_t hotspot_x,
                          int32_t hotspot_y) {
    struct server_surface *surface = server_surface_from_resource(surface_resource);

    if (surface->role != ROLE_NONE && surface->role != ROLE_CURSOR) {
        wl_resource_post_error(resource, WL_POINTER_ERROR_ROLE,
                               "surface provided to wl_pointer.set_cursor has a non-cursor role");
        return;
    }

    // We do not care about what the client wants to set the cursor to. GLFW's Wayland backend has
    // buggy cursor behavior and we want to configure it ourselves anyway.
}

static void
handle_pointer_release(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
pointer_destroy(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
}

static const struct wl_pointer_interface pointer_impl = {
    .set_cursor = handle_pointer_set_cursor,
    .release = handle_pointer_release,
};

static void
handle_keyboard_release(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
keyboard_destroy(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
}

static const struct wl_keyboard_interface keyboard_impl = {
    .release = handle_keyboard_release,
};

static void
handle_seat_get_pointer(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_seat *seat = server_seat_from_resource(resource);

    struct wl_resource *pointer_resource =
        wl_resource_create(client, &wl_pointer_interface, wl_resource_get_version(resource), id);
    if (!pointer_resource) {
        wl_client_post_no_memory(client);
        return;
    }

    wl_resource_set_implementation(pointer_resource, &pointer_impl, seat, pointer_destroy);

    wl_list_insert(&seat->pointers, wl_resource_get_link(pointer_resource));

    if (has_focus(seat, pointer_resource)) {
        send_pointer_enter(seat, pointer_resource);
    }
}

static void
handle_seat_get_keyboard(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_seat *seat = server_seat_from_resource(resource);

    struct wl_resource *keyboard_resource =
        wl_resource_create(client, &wl_keyboard_interface, wl_resource_get_version(resource), id);
    if (!keyboard_resource) {
        wl_client_post_no_memory(client);
        return;
    }

    wl_resource_set_implementation(keyboard_resource, &keyboard_impl, seat, keyboard_destroy);

    wl_list_insert(&seat->keyboards, wl_resource_get_link(keyboard_resource));

    // TODO: allow keymap and repeat info configuration
    wl_keyboard_send_keymap(keyboard_resource, seat->remote.keymap_format, seat->remote.keymap_fd,
                            seat->remote.keymap_size);
    wl_keyboard_send_repeat_info(keyboard_resource, 30, 400);
    if (has_focus(seat, keyboard_resource)) {
        send_keyboard_enter(seat, keyboard_resource);
    }
}

static void
handle_seat_get_touch(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    // Touch support is not implemented.
    wl_resource_post_error(resource, WL_SEAT_ERROR_MISSING_CAPABILITY,
                           "touch capability never advertised on this seat");
}

static void
handle_seat_release(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
seat_destroy(struct wl_resource *resource) {
    // Unused.
}

static const struct wl_seat_interface seat_impl = {
    .get_pointer = handle_seat_get_pointer,
    .get_keyboard = handle_seat_get_keyboard,
    .get_touch = handle_seat_get_touch,
    .release = handle_seat_release,
};

static void
handle_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= VERSION);
    struct server_seat *seat = data;

    struct wl_resource *resource = wl_resource_create(client, &wl_seat_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }

    wl_resource_set_implementation(resource, &seat_impl, seat, seat_destroy);

    wl_seat_send_capabilities(resource, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
    if (version >= WL_SEAT_NAME_SINCE_VERSION) {
        wl_seat_send_name(resource, "Waywall Seat");
    }
}

struct server_seat *
server_seat_create(struct server *server, struct wl_seat *remote) {
    struct server_seat *seat = calloc(1, sizeof(*seat));
    if (!seat) {
        LOG(LOG_ERROR, "failed to allocate server_seat");
        return NULL;
    }

    seat->on_destroy.notify = on_view_destroy;
    seat->remote.keymap_fd = -1;

    seat->global =
        wl_global_create(server->display, &wl_seat_interface, VERSION, seat, handle_bind);

    seat->display_destroy.notify = on_display_destroy;

    wl_display_add_destroy_listener(server->display, &seat->display_destroy);

    return seat;
}

void
server_seat_set_input_focus(struct server_seat *seat, struct server_view *view) {
    struct wl_resource *pointer_resource, *keyboard_resource;

    if (seat->input_focus) {
        wl_list_remove(&seat->on_destroy.link);

        struct server_surface *surface = server_view_get_surface(seat->input_focus);
        uint32_t time = current_time();

        wl_resource_for_each(pointer_resource, &seat->pointers) {
            if (!has_focus(seat, pointer_resource)) {
                continue;
            }

            for (size_t i = 0; i < seat->remote.buttons.count; i++) {
                wl_pointer_send_button(pointer_resource, next_serial(pointer_resource), time,
                                       seat->remote.buttons.data[i],
                                       WL_POINTER_BUTTON_STATE_RELEASED);
            }
            wl_pointer_send_leave(pointer_resource, next_serial(pointer_resource),
                                  surface->resource);
        }

        wl_resource_for_each(keyboard_resource, &seat->keyboards) {
            if (!has_focus(seat, keyboard_resource)) {
                continue;
            }

            for (size_t i = 0; i < seat->remote.keys.count; i++) {
                wl_keyboard_send_key(keyboard_resource, next_serial(keyboard_resource), time,
                                     seat->remote.keys.data[i], WL_KEYBOARD_KEY_STATE_RELEASED);
            }
            wl_keyboard_send_leave(keyboard_resource, next_serial(keyboard_resource),
                                   surface->resource);
        }
    }

    seat->input_focus = view;

    wl_resource_for_each(pointer_resource, &seat->pointers) {
        if (has_focus(seat, pointer_resource)) {
            send_pointer_enter(seat, pointer_resource);
        }
    }

    wl_resource_for_each(keyboard_resource, &seat->keyboards) {
        if (has_focus(seat, keyboard_resource)) {
            send_keyboard_enter(seat, keyboard_resource);
        }
    }

    wl_signal_add(&view->events.destroy, &seat->on_destroy);
}

void
server_seat_set_remote(struct server_seat *seat, struct wl_seat *remote) {
    if (seat->remote.seat == remote) {
        return;
    }

    if (seat->remote.keyboard) {
        wl_keyboard_release(seat->remote.keyboard);
    }
    if (seat->remote.pointer) {
        wl_pointer_release(seat->remote.pointer);
    }
    if (seat->remote.seat) {
        wl_seat_release(seat->remote.seat);
    }

    seat->remote.seat = remote;
    wl_seat_add_listener(remote, &seat_listener, seat);
}

struct server_seat *
server_seat_from_resource(struct wl_resource *resource) {
    ww_assert(wl_resource_instance_of(resource, &wl_seat_interface, &seat_impl) ||
              wl_resource_instance_of(resource, &wl_pointer_interface, &pointer_impl) ||
              wl_resource_instance_of(resource, &wl_keyboard_interface, &keyboard_impl));
    return wl_resource_get_user_data(resource);
}
