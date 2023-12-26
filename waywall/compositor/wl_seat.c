#include "compositor/wl_seat.h"
#include "compositor/server.h"
#include "compositor/wl_compositor.h"
#include "util.h"
#include <wayland-client.h>
#include <wayland-server.h>

/*
 *  Most keyboard and mouse events are implemented. Touch functionality is not implemented because
 *  GLFW does not make use of it.
 */

#define VERSION 8

static void
remote_seat_destroy(struct remote_seat *remote_seat) {
    if (remote_seat->pointer) {
        wl_pointer_release(remote_seat->pointer);
    }
    if (remote_seat->keyboard) {
        wl_keyboard_release(remote_seat->keyboard);
    }
    wl_seat_release(remote_seat->seat);

    free(remote_seat);
}

static void
on_pointer_enter(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
                 struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y) {
    struct remote_seat *remote_seat = data;

    remote_seat->last_pointer_enter = serial;
    remote_seat->parent->pointer_x = wl_fixed_to_double(surface_x);
    remote_seat->parent->pointer_y = wl_fixed_to_double(surface_y);

    struct server_surface *input_focus = remote_seat->parent->input_focus;

    if (!input_focus) {
        return;
    }

    struct server_pointer *pointer;
    wl_list_for_each (pointer, &remote_seat->parent->pointers, link) {
        if (wl_resource_get_client(pointer->resource) !=
            wl_resource_get_client(input_focus->resource)) {
            continue;
        }

        // TODO:
    }
}

static void
on_pointer_leave(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
                 struct wl_surface *surface) {
    // Unused.
}

static void
on_pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time, wl_fixed_t surface_x,
                  wl_fixed_t surface_y) {
    // TODO:
}

static void
on_pointer_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial, uint32_t time,
                  uint32_t button, uint32_t state) {
    struct remote_seat *remote_seat = data;
    struct server_surface *input_focus = remote_seat->parent->input_focus;

    if (!input_focus) {
        return;
    }

    struct server_pointer *pointer;
    wl_list_for_each (pointer, &remote_seat->parent->pointers, link) {
        if (wl_resource_get_client(pointer->resource) !=
            wl_resource_get_client(input_focus->resource)) {
            continue;
        }

        wl_pointer_send_button(pointer->resource, next_serial(pointer->resource), current_time(),
                               button, state);
    }
}

static void
on_pointer_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time, uint32_t axis,
                wl_fixed_t value) {
    struct remote_seat *remote_seat = data;
    struct server_surface *input_focus = remote_seat->parent->input_focus;

    if (!input_focus) {
        return;
    }

    struct server_pointer *pointer;
    wl_list_for_each (pointer, &remote_seat->parent->pointers, link) {
        if (wl_resource_get_client(pointer->resource) !=
            wl_resource_get_client(input_focus->resource)) {
            continue;
        }

        wl_pointer_send_axis(pointer->resource, current_time(), axis, value);
    }
}

static void
on_pointer_frame(void *data, struct wl_pointer *wl_pointer) {
    // Not used by GLFW.
}

static void
on_pointer_axis_source(void *data, struct wl_pointer *wl_pointer, uint32_t axis_source) {
    // Not used by GLFW.
}

static void
on_pointer_axis_stop(void *data, struct wl_pointer *wl_pointer, uint32_t time, uint32_t axis) {
    // Not used by GLFW.
}

static void
on_pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer, uint32_t axis,
                         int32_t discrete) {
    // Not used by GLFW.
}

static void
on_pointer_axis_value120(void *data, struct wl_pointer *wl_pointer, uint32_t axis,
                         int32_t value120) {
    // Not used by GLFW.
}

static void
on_pointer_axis_relative_direction(void *data, struct wl_pointer *wl_pointer, uint32_t axis,
                                   uint32_t direction) {
    // Not used by GLFW.
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
    .axis_relative_direction = on_pointer_axis_relative_direction,
};

static void
on_keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard, uint32_t format, int32_t fd,
                   uint32_t size) {
    struct remote_seat *remote_seat = data;

    if (remote_seat->parent->keymap_fd >= 0) {
        close(remote_seat->parent->keymap_fd);
    }

    remote_seat->parent->keymap_format = format;
    remote_seat->parent->keymap_fd = fd;
    remote_seat->parent->keymap_size = size;

    struct server_keyboard *keyboard;
    wl_list_for_each (keyboard, &remote_seat->parent->keyboards, link) {
        // TODO: don't send if user has custom keymap in config
        wl_keyboard_send_keymap(keyboard->resource, format, fd, size);
    }
}

static void
on_keyboard_enter(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
                  struct wl_surface *surface, struct wl_array *keys) {
    struct remote_seat *remote_seat = data;

    ww_assert(remote_seat->parent->num_pressed_keys == 0);

    uint32_t *key;
    wl_array_for_each(key, keys) {
        ww_assert(remote_seat->parent->num_pressed_keys <
                  ARRAY_LEN(remote_seat->parent->pressed_keys));

        remote_seat->parent->pressed_keys[remote_seat->parent->num_pressed_keys++] = *key;
    }

    struct server_surface *input_focus = remote_seat->parent->input_focus;

    if (!input_focus) {
        return;
    }

    struct server_keyboard *keyboard;
    wl_list_for_each (keyboard, &remote_seat->parent->keyboards, link) {
        if (wl_resource_get_client(keyboard->resource) !=
            wl_resource_get_client(input_focus->resource)) {
            continue;
        }

        for (size_t i = 0; i < remote_seat->parent->num_pressed_keys; i++) {
            wl_keyboard_send_key(keyboard->resource, next_serial(keyboard->resource),
                                 current_time(), remote_seat->parent->pressed_keys[i],
                                 WL_KEYBOARD_KEY_STATE_PRESSED);
        }
    }
}

static void
on_keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                  struct wl_surface *surface) {
    struct remote_seat *remote_seat = data;

    struct server_surface *input_focus = remote_seat->parent->input_focus;
    if (input_focus) {
        struct server_keyboard *keyboard;
        wl_list_for_each (keyboard, &remote_seat->parent->keyboards, link) {
            if (wl_resource_get_client(keyboard->resource) !=
                wl_resource_get_client(input_focus->resource)) {
                continue;
            }

            for (size_t i = 0; i < remote_seat->parent->num_pressed_keys; i++) {
                wl_keyboard_send_key(keyboard->resource, next_serial(keyboard->resource),
                                     current_time(), remote_seat->parent->pressed_keys[i],
                                     WL_KEYBOARD_KEY_STATE_RELEASED);
            }
        }
    }

    remote_seat->parent->num_pressed_keys = 0;
}

static void
on_keyboard_key(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, uint32_t time,
                uint32_t key, uint32_t state) {
    struct remote_seat *remote_seat = data;

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        ww_assert(remote_seat->parent->num_pressed_keys <
                  ARRAY_LEN(remote_seat->parent->pressed_keys));

        remote_seat->parent->pressed_keys[remote_seat->parent->num_pressed_keys++] = key;
    } else {
        bool found = false;
        for (size_t i = 0; i < remote_seat->parent->num_pressed_keys; i++) {
            if (remote_seat->parent->pressed_keys[i] == key) {
                memmove(remote_seat->parent->pressed_keys + i,
                        remote_seat->parent->pressed_keys + i + 1,
                        remote_seat->parent->num_pressed_keys - i - 1);
                remote_seat->parent->num_pressed_keys--;

                found = true;
                break;
            }
        }

        if (!found) {
            LOG(LOG_ERROR, "spurious key release event (keycode %" PRIu32 ")", key);
            return;
        }
    }

    struct server_surface *input_focus = remote_seat->parent->input_focus;

    if (!input_focus) {
        return;
    }

    struct server_keyboard *keyboard;
    wl_list_for_each (keyboard, &remote_seat->parent->keyboards, link) {
        if (wl_resource_get_client(keyboard->resource) !=
            wl_resource_get_client(input_focus->resource)) {
            continue;
        }

        wl_keyboard_send_key(keyboard->resource, next_serial(keyboard->resource), current_time(),
                             key, state);
    }
}

static void
on_keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
                      uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked,
                      uint32_t group) {
    struct remote_seat *remote_seat = data;

    remote_seat->parent->mods_depressed = mods_depressed;
    remote_seat->parent->mods_latched = mods_latched;
    remote_seat->parent->mods_locked = mods_locked;
    remote_seat->parent->group = group;

    struct server_surface *input_focus = remote_seat->parent->input_focus;

    if (!input_focus) {
        return;
    }

    struct server_keyboard *keyboard;
    wl_list_for_each (keyboard, &remote_seat->parent->keyboards, link) {
        if (wl_resource_get_client(keyboard->resource) !=
            wl_resource_get_client(input_focus->resource)) {
            continue;
        }

        wl_keyboard_send_modifiers(keyboard->resource, next_serial(keyboard->resource),
                                   mods_depressed, mods_latched, mods_locked, group);
    }
}

static void
on_keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay) {
    // Unused. We allow the user to configure their own repeat info to be passed to clients.
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
    struct remote_seat *remote_seat = data;

    bool has_pointer = (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0;
    bool has_keyboard = (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0;

    if (has_pointer && !remote_seat->pointer) {
        remote_seat->pointer = wl_seat_get_pointer(wl_seat);
        wl_pointer_add_listener(remote_seat->pointer, &pointer_listener, remote_seat);
    } else if (!has_pointer && remote_seat->pointer) {
        wl_pointer_release(remote_seat->pointer);
        remote_seat->pointer = NULL;
    }

    if (has_keyboard && !remote_seat->keyboard) {
        remote_seat->keyboard = wl_seat_get_keyboard(wl_seat);
        wl_keyboard_add_listener(remote_seat->keyboard, &keyboard_listener, remote_seat);
    } else if (!has_keyboard && remote_seat->keyboard) {
        wl_keyboard_release(remote_seat->keyboard);
        remote_seat->keyboard = NULL;
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

    if (seat->remote) {
        remote_seat_destroy(seat->remote);
    }

    wl_global_destroy(seat->global);

    free(seat);
}

static void
handle_pointer_set_cursor(struct wl_client *client, struct wl_resource *resource, uint32_t serial,
                          struct wl_resource *surface_resource, int32_t hotspot_x,
                          int32_t hotspot_y) {
    wl_client_post_implementation_error(client, "TODO");
}

static void
handle_pointer_release(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
pointer_destroy(struct wl_resource *resource) {
    struct server_pointer *pointer = server_pointer_from_resource(resource);

    wl_list_remove(&pointer->link);
    free(pointer);
}

static const struct wl_pointer_interface pointer_impl = {
    .set_cursor = handle_pointer_set_cursor,
    .release = handle_pointer_release,
};

struct server_pointer *
server_pointer_from_resource(struct wl_resource *resource) {
    ww_assert(wl_resource_instance_of(resource, &wl_pointer_interface, &pointer_impl));
    return wl_resource_get_user_data(resource);
}

static void
handle_keyboard_release(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
keyboard_destroy(struct wl_resource *resource) {
    struct server_keyboard *keyboard = server_keyboard_from_resource(resource);

    wl_list_remove(&keyboard->link);
    free(keyboard);
}

static const struct wl_keyboard_interface keyboard_impl = {
    .release = handle_keyboard_release,
};

struct server_keyboard *
server_keyboard_from_resource(struct wl_resource *resource) {
    ww_assert(wl_resource_instance_of(resource, &wl_keyboard_interface, &keyboard_impl));
    return wl_resource_get_user_data(resource);
}

static void
handle_seat_get_pointer(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_seat *seat = server_seat_from_resource(resource);

    struct server_pointer *pointer = calloc(1, sizeof(*pointer));
    if (!pointer) {
        wl_client_post_no_memory(client);
        return;
    }

    struct wl_resource *pointer_resource =
        wl_resource_create(client, &wl_pointer_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(pointer_resource, &pointer_impl, pointer, pointer_destroy);
    wl_list_insert(&seat->pointers, &pointer->link);

    pointer->parent = seat;
    pointer->resource = pointer_resource;

    // TODO: send enter
}

static void
handle_seat_get_keyboard(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_seat *seat = server_seat_from_resource(resource);

    struct server_keyboard *keyboard = calloc(1, sizeof(*keyboard));
    if (!keyboard) {
        wl_client_post_no_memory(client);
        return;
    }

    struct wl_resource *keyboard_resource =
        wl_resource_create(client, &wl_keyboard_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(keyboard_resource, &keyboard_impl, keyboard, keyboard_destroy);
    wl_list_insert(&seat->keyboards, &keyboard->link);

    keyboard->parent = seat;
    keyboard->resource = keyboard_resource;

    // TODO: send events
}

static void
handle_seat_get_touch(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    // The touch capability is never provided on any seat, so using this function is a protocol
    // violation.

    // No relevant clients should make use of wl_touch.
    wl_resource_post_error(resource, WL_SEAT_ERROR_MISSING_CAPABILITY,
                           "no touch capability ever existed on this seat");
}

static void
handle_seat_release(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
seat_destroy(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
}

static const struct wl_seat_interface seat_impl = {
    .get_pointer = handle_seat_get_pointer,
    .get_keyboard = handle_seat_get_keyboard,
    .get_touch = handle_seat_get_touch,
    .release = handle_seat_release,
};

struct server_seat *
server_seat_from_resource(struct wl_resource *resource) {
    ww_assert(wl_resource_instance_of(resource, &wl_seat_interface, &seat_impl));
    return wl_resource_get_user_data(resource);
}

static void
handle_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= VERSION);
    struct server_seat *seat = data;

    struct wl_resource *resource = wl_resource_create(client, &wl_seat_interface, version, id);
    wl_resource_set_implementation(resource, &seat_impl, seat, seat_destroy);

    wl_seat_send_capabilities(resource, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
}

struct server_seat *
server_seat_create(struct server *server, struct wl_seat *remote) {
    struct server_seat *seat = calloc(1, sizeof(*seat));
    if (!seat) {
        LOG(LOG_ERROR, "failed to allocate server_seat");
        return NULL;
    }

    seat->keymap_fd = -1;

    struct remote_seat *remote_seat = calloc(1, sizeof(*remote_seat));
    if (!remote_seat) {
        LOG(LOG_ERROR, "failed to allocate remote_seat");
        free(seat);
        return NULL;
    }

    remote_seat->parent = seat;
    remote_seat->seat = remote;
    wl_seat_add_listener(remote_seat->seat, &seat_listener, remote_seat);

    seat->global =
        wl_global_create(server->display, &wl_seat_interface, VERSION, seat, handle_bind);

    seat->display_destroy.notify = on_display_destroy;

    wl_display_add_destroy_listener(server->display, &seat->display_destroy);

    return seat;
}
