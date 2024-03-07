#include "server/wl_seat.h"
#include "server/serial.h"
#include "server/server.h"
#include "server/wl_compositor.h"
#include "util.h"
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server.h>

#define SRV_SEAT_VERSION 6

static void
cursor_role_commit(struct wl_resource *resource) {
    // Unused.
}

static void
cursor_role_destroy(struct wl_resource *resource) {
    // Unused.
}

static const struct server_surface_role cursor_role = {
    .name = "cursor",

    .commit = cursor_role_commit,
    .destroy = cursor_role_destroy,
};

static uint32_t
current_time() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)((uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000);
}

static void
get_pointer_offset(struct server_seat_g *seat_g, double *x, double *y) {
    ww_assert(seat_g->input_focus);

    *x = seat_g->ptr_state.x - (double)seat_g->input_focus->x;
    *y = seat_g->ptr_state.y - (double)seat_g->input_focus->y;
}

static void
send_keyboard_enter(struct server_seat_g *seat_g) {
    ww_assert(seat_g->input_focus);

    struct wl_array keys;
    wl_array_init(&keys);
    uint32_t *data = wl_array_add(&keys, sizeof(uint32_t) * seat_g->kb_state.pressed.len);
    ww_assert(data);
    for (size_t i = 0; i < seat_g->kb_state.pressed.len; i++) {
        data[i] = seat_g->kb_state.pressed.data[i];
    }

    struct wl_client *client = wl_resource_get_client(seat_g->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat_g->keyboards) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        wl_keyboard_send_enter(resource, next_serial(resource),
                               seat_g->input_focus->surface->resource, &keys);
    }

    wl_array_release(&keys);
}

static void
send_keyboard_key(struct server_seat_g *seat_g, uint32_t key, enum wl_keyboard_key_state state) {
    if (!seat_g->input_focus) {
        return;
    }

    struct wl_client *client = wl_resource_get_client(seat_g->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat_g->keyboards) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        wl_keyboard_send_key(resource, next_serial(resource), current_time(), key, state);
    }
}

static void
send_keyboard_leave(struct server_seat_g *seat_g) {
    if (!seat_g->input_focus) {
        return;
    }

    struct wl_client *client = wl_resource_get_client(seat_g->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat_g->keyboards) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        wl_keyboard_send_leave(resource, next_serial(resource),
                               seat_g->input_focus->surface->resource);
    }
}

static void
send_keyboard_modifiers(struct server_seat_g *seat_g) {
    if (!seat_g->input_focus) {
        return;
    }

    struct wl_client *client = wl_resource_get_client(seat_g->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat_g->keyboards) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        wl_keyboard_send_modifiers(resource, next_serial(resource), seat_g->kb_state.mods_depressed,
                                   seat_g->kb_state.mods_latched, seat_g->kb_state.mods_locked,
                                   seat_g->kb_state.group);
    }
}

static void
send_pointer_enter(struct server_seat_g *seat_g) {
    ww_assert(seat_g->input_focus);

    double x, y;
    get_pointer_offset(seat_g, &x, &y);

    struct wl_client *client = wl_resource_get_client(seat_g->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat_g->pointers) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        wl_pointer_send_enter(resource, next_serial(resource),
                              seat_g->input_focus->surface->resource, wl_fixed_from_double(x),
                              wl_fixed_from_double(y));
    }
}

static void
send_pointer_leave(struct server_seat_g *seat_g) {
    if (!seat_g->input_focus) {
        return;
    }

    struct wl_client *client = wl_resource_get_client(seat_g->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat_g->pointers) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        wl_pointer_send_leave(resource, next_serial(resource),
                              seat_g->input_focus->surface->resource);
    }
}

static void
on_keyboard_enter(void *data, struct wl_keyboard *wl, uint32_t serial, struct wl_surface *surface,
                  struct wl_array *keys) {
    // Unused.
}

static void
on_keyboard_key(void *data, struct wl_keyboard *wl, uint32_t serial, uint32_t time, uint32_t key,
                uint32_t state) {
    struct server_seat_g *seat_g = data;

    // Update the pressed keys array.
    switch ((enum wl_keyboard_key_state)state) {
    case WL_KEYBOARD_KEY_STATE_PRESSED:
        for (size_t i = 0; i < seat_g->kb_state.pressed.len; i++) {
            if (seat_g->kb_state.pressed.data[i] == key) {
                ww_log(LOG_WARN, "duplicate key press event received");
                return;
            }
        }

        if (seat_g->kb_state.pressed.len == seat_g->kb_state.pressed.cap) {
            uint32_t *new_data = realloc(seat_g->kb_state.pressed.data,
                                         sizeof(uint32_t) * seat_g->kb_state.pressed.cap * 2);
            if (!new_data) {
                ww_log(LOG_WARN, "failed to reallocate pressed keys buffer - input dropped");
                return;
            }

            seat_g->kb_state.pressed.data = new_data;
            seat_g->kb_state.pressed.cap *= 2;
        }

        seat_g->kb_state.pressed.data[seat_g->kb_state.pressed.len++] = key;
        break;
    case WL_KEYBOARD_KEY_STATE_RELEASED:
        for (size_t i = 0; i < seat_g->kb_state.pressed.len; i++) {
            if (seat_g->kb_state.pressed.data[i] != key) {
                continue;
            }

            memmove(seat_g->kb_state.pressed.data + i, seat_g->kb_state.pressed.data + i + 1,
                    sizeof(uint32_t) * (seat_g->kb_state.pressed.len - i - 1));
            seat_g->kb_state.pressed.len--;
            break;
        }
        break;
    }

    if (seat_g->listener) {
        bool consumed = seat_g->listener->key(seat_g->listener_data, key,
                                              state == WL_KEYBOARD_KEY_STATE_PRESSED);
        if (consumed) {
            return;
        }
    }
    send_keyboard_key(seat_g, key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
}

static void
on_keyboard_keymap(void *data, struct wl_keyboard *wl, uint32_t format, int32_t fd, uint32_t size) {
    struct server_seat_g *seat_g = data;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        ww_log(LOG_WARN, "received keymap of unknown type %" PRIu32, format);
        return;
    }

    if (seat_g->kb_state.keymap_fd >= 0) {
        close(seat_g->kb_state.keymap_fd);
    }
    seat_g->kb_state.keymap_fd = fd;
    seat_g->kb_state.keymap_size = size;

    if (seat_g->listener) {
        seat_g->listener->keymap(seat_g->listener_data, fd, size);
    }
}

static void
on_keyboard_leave(void *data, struct wl_keyboard *wl, uint32_t serial, struct wl_surface *surface) {
    struct server_seat_g *seat_g = data;

    for (size_t i = 0; i < seat_g->kb_state.pressed.len; i++) {
        send_keyboard_key(seat_g, seat_g->kb_state.pressed.data[i], WL_KEYBOARD_KEY_STATE_RELEASED);
    }
    seat_g->kb_state.pressed.len = 0;

    seat_g->kb_state.mods_depressed = 0;
    seat_g->kb_state.mods_latched = 0;
    seat_g->kb_state.mods_locked = 0;
    seat_g->kb_state.group = 0;
    send_keyboard_modifiers(seat_g);
}

static void
on_keyboard_modifiers(void *data, struct wl_keyboard *wl, uint32_t serial, uint32_t mods_depressed,
                      uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
    struct server_seat_g *seat_g = data;

    seat_g->kb_state.mods_depressed = mods_depressed;
    seat_g->kb_state.mods_latched = mods_latched;
    seat_g->kb_state.mods_locked = mods_locked;
    seat_g->kb_state.group = group;
    send_keyboard_modifiers(seat_g);
}

static void
on_keyboard_repeat_info(void *data, struct wl_keyboard *wl, int32_t rate, int32_t delay) {
    struct server_seat_g *seat_g = data;

    seat_g->kb_state.repeat_rate = rate;
    seat_g->kb_state.repeat_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .enter = on_keyboard_enter,
    .key = on_keyboard_key,
    .keymap = on_keyboard_keymap,
    .leave = on_keyboard_leave,
    .modifiers = on_keyboard_modifiers,
    .repeat_info = on_keyboard_repeat_info,
};

static void
on_pointer_axis(void *data, struct wl_pointer *wl, uint32_t time, uint32_t axis, wl_fixed_t value) {
    struct server_seat_g *seat_g = data;

    if (!seat_g->input_focus) {
        return;
    }

    struct wl_client *client = wl_resource_get_client(seat_g->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat_g->pointers) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        wl_pointer_send_axis(resource, current_time(), axis, value);
    }
}

static void
on_pointer_axis_discrete(void *data, struct wl_pointer *wl, uint32_t axis, int32_t discrete) {
    struct server_seat_g *seat_g = data;

    if (!seat_g->input_focus) {
        return;
    }

    struct wl_client *client = wl_resource_get_client(seat_g->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat_g->pointers) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        if (wl_resource_get_version(resource) >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION) {
            wl_pointer_send_axis_discrete(resource, axis, discrete);
        }
    }
}

static void
on_pointer_axis_source(void *data, struct wl_pointer *wl, uint32_t source) {
    struct server_seat_g *seat_g = data;

    if (!seat_g->input_focus) {
        return;
    }

    struct wl_client *client = wl_resource_get_client(seat_g->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat_g->pointers) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        if (wl_resource_get_version(resource) >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION) {
            wl_pointer_send_axis_source(resource, source);
        }
    }
}

static void
on_pointer_axis_stop(void *data, struct wl_pointer *wl, uint32_t time, uint32_t axis) {
    struct server_seat_g *seat_g = data;

    if (!seat_g->input_focus) {
        return;
    }

    struct wl_client *client = wl_resource_get_client(seat_g->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat_g->pointers) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        if (wl_resource_get_version(resource) >= WL_POINTER_AXIS_STOP_SINCE_VERSION) {
            wl_pointer_send_axis_stop(resource, current_time(), axis);
        }
    }
}

static void
on_pointer_button(void *data, struct wl_pointer *wl, uint32_t serial, uint32_t time,
                  uint32_t button, uint32_t state) {
    struct server_seat_g *seat_g = data;

    if (seat_g->listener) {
        bool consumed = seat_g->listener->button(seat_g->listener_data, button,
                                                 state == WL_POINTER_BUTTON_STATE_PRESSED);
        if (consumed) {
            return;
        }
    }

    if (!seat_g->input_focus) {
        return;
    }

    struct wl_client *client = wl_resource_get_client(seat_g->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat_g->pointers) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        wl_pointer_send_button(resource, next_serial(resource), current_time(), button, state);
    }
}

static void
on_pointer_enter(void *data, struct wl_pointer *wl, uint32_t serial, struct wl_surface *surface,
                 wl_fixed_t surface_x, wl_fixed_t surface_y) {
    // Unused.
}

static void
on_pointer_frame(void *data, struct wl_pointer *wl) {
    struct server_seat_g *seat_g = data;

    if (!seat_g->input_focus) {
        return;
    }

    struct wl_client *client = wl_resource_get_client(seat_g->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat_g->pointers) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        if (wl_resource_get_version(resource) >= WL_POINTER_FRAME_SINCE_VERSION) {
            wl_pointer_send_frame(resource);
        }
    }
}

static void
on_pointer_leave(void *data, struct wl_pointer *wl, uint32_t serial, struct wl_surface *surface) {
    // Unused.
}

static void
on_pointer_motion(void *data, struct wl_pointer *wl, uint32_t time, wl_fixed_t surface_x,
                  wl_fixed_t surface_y) {
    struct server_seat_g *seat_g = data;

    seat_g->ptr_state.x = wl_fixed_to_double(surface_x);
    seat_g->ptr_state.y = wl_fixed_to_double(surface_y);

    if (seat_g->listener) {
        seat_g->listener->motion(seat_g->listener_data, seat_g->ptr_state.x, seat_g->ptr_state.y);
    }

    if (!seat_g->input_focus) {
        return;
    }

    double x, y;
    get_pointer_offset(seat_g, &x, &y);

    struct wl_client *client = wl_resource_get_client(seat_g->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat_g->pointers) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        wl_pointer_send_motion(resource, current_time(), wl_fixed_from_double(x),
                               wl_fixed_from_double(y));
    }
}

static const struct wl_pointer_listener pointer_listener = {
    .axis = on_pointer_axis,
    .axis_discrete = on_pointer_axis_discrete,
    .axis_source = on_pointer_axis_source,
    .axis_stop = on_pointer_axis_stop,
    .button = on_pointer_button,
    .enter = on_pointer_enter,
    .frame = on_pointer_frame,
    .leave = on_pointer_leave,
    .motion = on_pointer_motion,

    .axis_value120 = NULL,           // introduced in v8 (using v5)
    .axis_relative_direction = NULL, // introduced in v9 (using v5)
};

static void
process_seat_caps(struct server_seat_g *seat_g, uint32_t caps) {
    bool had_kb = !!seat_g->keyboard;
    bool has_kb = caps & WL_SEAT_CAPABILITY_KEYBOARD;
    if (had_kb && !has_kb) {
        wl_keyboard_release(seat_g->keyboard);
        seat_g->keyboard = NULL;
    } else if (!had_kb && has_kb) {
        seat_g->keyboard = wl_seat_get_keyboard(seat_g->remote);
        ww_assert(seat_g->keyboard);
        wl_keyboard_add_listener(seat_g->keyboard, &keyboard_listener, seat_g);
    }

    bool had_ptr = !!seat_g->pointer;
    bool has_ptr = caps & WL_SEAT_CAPABILITY_POINTER;
    if (had_ptr && !has_ptr) {
        wl_pointer_release(seat_g->pointer);
        seat_g->pointer = NULL;

        wl_signal_emit_mutable(&seat_g->events.pointer, NULL);
    } else if (!had_ptr && has_ptr) {
        seat_g->pointer = wl_seat_get_pointer(seat_g->remote);
        ww_assert(seat_g->pointer);
        wl_pointer_add_listener(seat_g->pointer, &pointer_listener, seat_g);

        wl_signal_emit_mutable(&seat_g->events.pointer, seat_g->pointer);
    }
}

static void
on_view_destroy(struct wl_listener *listener, void *data) {
    struct server_seat_g *seat_g = wl_container_of(listener, seat_g, on_view_destroy);

    server_seat_g_set_input_focus(seat_g, NULL);
}

static void
on_seat_caps(struct wl_listener *listener, void *data) {
    struct server_seat_g *seat_g = wl_container_of(listener, seat_g, on_seat_caps);
    uint32_t *caps = data;

    process_seat_caps(seat_g, *caps);
}

static void
init_seat(struct server_seat_g *seat_g, struct wl_seat *remote) {
    if (seat_g->remote) {
        wl_seat_release(seat_g->remote);
        if (seat_g->keyboard) {
            wl_keyboard_release(seat_g->keyboard);
        }
        if (seat_g->pointer) {
            wl_pointer_release(seat_g->pointer);
        }

        send_keyboard_leave(seat_g);
        send_pointer_leave(seat_g);
    }

    seat_g->remote = remote;
}

static void
keyboard_resource_destroy(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
}

static void
keyboard_release(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct wl_keyboard_interface keyboard_impl = {
    .release = keyboard_release,
};

static void
pointer_resource_destroy(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
}

static void
pointer_release(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
pointer_set_cursor(struct wl_client *client, struct wl_resource *resource, uint32_t serial,
                   struct wl_resource *surface_resource, int32_t hotspot_x, int32_t hotspot_y) {
    // We do not care about what clients want to set the cursor to. However, we will mark surfaces
    // with the correct roles anyway.
    struct server_surface *surface = server_surface_from_resource(surface_resource);

    if (server_surface_set_role(surface, &cursor_role, NULL) != 0) {
        wl_resource_post_error(
            resource, WL_POINTER_ERROR_ROLE,
            "cannot call wl_pointer.set_cursor with a surface that has another role");
        return;
    }
}

static const struct wl_pointer_interface pointer_impl = {
    .release = pointer_release,
    .set_cursor = pointer_set_cursor,
};

static void
seat_resource_destroy(struct wl_resource *resource) {
    // Unused.
}

static void
seat_get_keyboard(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_seat_g *seat_g = wl_resource_get_user_data(resource);

    struct wl_resource *keyboard_resource =
        wl_resource_create(client, &wl_keyboard_interface, wl_resource_get_version(resource), id);
    if (!keyboard_resource) {
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(keyboard_resource, &keyboard_impl, seat_g,
                                   keyboard_resource_destroy);

    wl_list_insert(&seat_g->keyboards, wl_resource_get_link(keyboard_resource));

    // TODO: send keymap, repeat info
}

static void
seat_get_pointer(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_seat_g *seat_g = wl_resource_get_user_data(resource);

    struct wl_resource *pointer_resource =
        wl_resource_create(client, &wl_pointer_interface, wl_resource_get_version(resource), id);
    if (!pointer_resource) {
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(pointer_resource, &pointer_impl, seat_g,
                                   pointer_resource_destroy);

    wl_list_insert(&seat_g->pointers, wl_resource_get_link(pointer_resource));
}

static void
seat_get_touch(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    wl_client_post_implementation_error(client, "wl_seat.get_touch is not supported");
}

static void
seat_release(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct wl_seat_interface seat_impl = {
    .get_keyboard = seat_get_keyboard,
    .get_pointer = seat_get_pointer,
    .get_touch = seat_get_touch,
    .release = seat_release,
};

static void
on_global_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SRV_SEAT_VERSION);

    struct server_seat_g *seat_g = data;

    struct wl_resource *resource = wl_resource_create(client, &wl_seat_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &seat_impl, seat_g, seat_resource_destroy);

    if (version >= WL_SEAT_NAME_SINCE_VERSION) {
        wl_seat_send_name(resource, "Waywall Seat");
    }
    wl_seat_send_capabilities(resource, WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_POINTER);
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_seat_g *seat_g = wl_container_of(listener, seat_g, on_display_destroy);

    wl_global_destroy(seat_g->global);

    if (seat_g->remote) {
        if (seat_g->keyboard) {
            wl_keyboard_release(seat_g->keyboard);
        }
        if (seat_g->pointer) {
            wl_pointer_release(seat_g->pointer);
        }
        wl_seat_release(seat_g->remote);
    }
    if (seat_g->kb_state.keymap_fd >= 0) {
        close(seat_g->kb_state.keymap_fd);
    }
    if (seat_g->kb_state.pressed.data) {
        free(seat_g->kb_state.pressed.data);
    }

    wl_list_remove(&seat_g->on_seat_caps.link);
    wl_list_remove(&seat_g->on_display_destroy.link);

    free(seat_g);
}

struct server_seat_g *
server_seat_g_create(struct server *server) {
    struct server_seat_g *seat_g = calloc(1, sizeof(*seat_g));
    if (!seat_g) {
        ww_log(LOG_ERROR, "failed to allocate server_seat_g");
        return NULL;
    }

    seat_g->global = wl_global_create(server->display, &wl_seat_interface, SRV_SEAT_VERSION, seat_g,
                                      on_global_bind);
    if (!seat_g->global) {
        ww_log(LOG_ERROR, "failed to allocate seat global");
        free(seat_g);
        return NULL;
    }

    seat_g->kb_state.keymap_fd = -1;
    seat_g->kb_state.pressed.cap = 8;
    seat_g->kb_state.pressed.data = calloc(seat_g->kb_state.pressed.cap, sizeof(uint32_t));
    if (!seat_g->kb_state.pressed.data) {
        ww_log(LOG_ERROR, "failed to allocate pressed keys array");
        wl_global_destroy(seat_g->global);
        free(seat_g);
        return NULL;
    }

    wl_list_init(&seat_g->keyboards);
    wl_list_init(&seat_g->pointers);

    wl_signal_init(&seat_g->events.pointer);

    ww_assert(server->backend.seat);
    init_seat(seat_g, server->backend.seat);
    process_seat_caps(seat_g, server->backend.seat_caps);

    seat_g->on_view_destroy.notify = on_view_destroy;

    seat_g->on_seat_caps.notify = on_seat_caps;
    wl_signal_add(&server->backend.events.seat_caps, &seat_g->on_seat_caps);

    seat_g->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &seat_g->on_display_destroy);

    return seat_g;
}

void
server_seat_g_set_input_focus(struct server_seat_g *seat_g, struct server_view *view) {
    if (seat_g->input_focus == view) {
        return;
    }

    if (seat_g->input_focus) {
        wl_list_remove(&seat_g->on_view_destroy.link);
    }

    send_keyboard_leave(seat_g);
    send_pointer_leave(seat_g);
    seat_g->input_focus = view;
    if (seat_g->input_focus) {
        send_keyboard_enter(seat_g);
        send_pointer_enter(seat_g);
        wl_signal_add(&view->events.destroy, &seat_g->on_view_destroy);
    }
}

void
server_seat_g_set_listener(struct server_seat_g *seat_g,
                           const struct server_seat_listener *listener, void *data) {
    ww_assert(!seat_g->listener);

    seat_g->listener = listener;
    seat_g->listener_data = data;
    if (seat_g->kb_state.keymap_fd >= 0) {
        seat_g->listener->keymap(seat_g->listener_data, seat_g->kb_state.keymap_fd,
                                 seat_g->kb_state.keymap_size);
    }
}
