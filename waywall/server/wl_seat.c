#define _GNU_SOURCE
#include <sys/mman.h>
#undef _GNU_SOURCE

#include "config.h"
#include "server/serial.h"
#include "server/server.h"
#include "server/wl_compositor.h"
#include "server/wl_seat.h"
#include "util.h"
#include <linux/input-event-codes.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>

#define SRV_SEAT_VERSION 6

static const char *mod_names[] = {
    XKB_MOD_NAME_SHIFT, XKB_MOD_NAME_CAPS,
    XKB_MOD_NAME_CTRL,  XKB_MOD_NAME_ALT,
    XKB_MOD_NAME_NUM,   "Mod3",
    XKB_MOD_NAME_LOGO,  "Mod5",
};

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
get_pointer_offset(struct server_seat *seat, double *x, double *y) {
    ww_assert(seat->input_focus);

    *x = seat->ptr_state.x - (double)seat->input_focus->x;
    *y = seat->ptr_state.y - (double)seat->input_focus->y;
}

static int
prepare_keymap(struct server_seat *seat) {
    char *keymap = xkb_keymap_get_as_string(seat->cfg->input.xkb_keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!keymap) {
        ww_log(LOG_ERROR, "failed to get XKB keymap as string");
        return 1;
    }

    seat->kb_state.local_keymap.size = strlen(keymap) + 1;
    seat->kb_state.local_keymap.fd = memfd_create("waywall-keymap", MFD_CLOEXEC);
    if (seat->kb_state.local_keymap.fd == -1) {
        ww_log_errno(LOG_ERROR, "failed to create keymap memfd");
        goto fail_memfd;
    }

    if (ftruncate(seat->kb_state.local_keymap.fd, seat->kb_state.local_keymap.size) == -1) {
        ww_log_errno(LOG_ERROR, "failed to expand keymap memfd");
        goto fail_truncate;
    }

    char *data = mmap(NULL, seat->kb_state.local_keymap.size + 1, PROT_READ | PROT_WRITE,
                      MAP_SHARED, seat->kb_state.local_keymap.fd, 0);
    if (data == MAP_FAILED) {
        ww_log_errno(LOG_ERROR, "failed to mmap keymap memfd");
        goto fail_mmap;
    }

    memcpy(data, keymap, seat->kb_state.local_keymap.size + 1);
    ww_assert(munmap(data, seat->kb_state.local_keymap.size + 1) == 0);

    free(keymap);
    return 0;

fail_mmap:
fail_truncate:
    close(seat->kb_state.local_keymap.fd);
    seat->kb_state.local_keymap.fd = -1;

fail_memfd:
    free(keymap);
    return 1;
}

static struct xkb_keymap *
prepare_remote_keymap(struct xkb_context *ctx, int32_t fd, int32_t size) {
    char *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
        ww_log_errno(LOG_ERROR, "failed to mmap keymap data");
        return NULL;
    }

    struct xkb_keymap *keymap =
        xkb_keymap_new_from_string(ctx, data, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_MAP_COMPILE_NO_FLAGS);

    ww_assert(munmap(data, size) == 0);
    return keymap;
}

static void
send_keyboard_enter(struct server_seat *seat) {
    ww_assert(seat->input_focus);

    struct wl_array keys;
    wl_array_init(&keys);
    uint32_t *data = wl_array_add(&keys, sizeof(uint32_t) * seat->kb_state.pressed.len);
    ww_assert(data);
    for (size_t i = 0; i < seat->kb_state.pressed.len; i++) {
        data[i] = seat->kb_state.pressed.data[i];
    }

    struct wl_client *client = wl_resource_get_client(seat->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat->keyboards) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        wl_keyboard_send_enter(resource, next_serial(resource),
                               seat->input_focus->surface->resource, &keys);
    }

    wl_array_release(&keys);
}

static void
send_keyboard_key(struct server_seat *seat, uint32_t key, enum wl_keyboard_key_state state) {
    if (!seat->input_focus) {
        return;
    }

    struct wl_client *client = wl_resource_get_client(seat->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat->keyboards) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        wl_keyboard_send_key(resource, next_serial(resource), current_time(), key, state);
    }
}

static void
send_keyboard_leave(struct server_seat *seat) {
    if (!seat->input_focus) {
        return;
    }

    struct wl_client *client = wl_resource_get_client(seat->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat->keyboards) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        wl_keyboard_send_leave(resource, next_serial(resource),
                               seat->input_focus->surface->resource);
    }
}

static void
send_keyboard_modifiers(struct server_seat *seat) {
    if (!seat->input_focus) {
        return;
    }

    struct wl_client *client = wl_resource_get_client(seat->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat->keyboards) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        wl_keyboard_send_modifiers(resource, next_serial(resource), seat->kb_state.mods.depressed,
                                   seat->kb_state.mods.latched, seat->kb_state.mods.locked,
                                   seat->kb_state.mods.group);
    }
}

static void
send_pointer_enter(struct server_seat *seat) {
    ww_assert(seat->input_focus);

    double x, y;
    get_pointer_offset(seat, &x, &y);

    struct wl_client *client = wl_resource_get_client(seat->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat->pointers) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        wl_pointer_send_enter(resource, next_serial(resource), seat->input_focus->surface->resource,
                              wl_fixed_from_double(x), wl_fixed_from_double(y));
    }
}

static void
send_pointer_leave(struct server_seat *seat) {
    if (!seat->input_focus) {
        return;
    }

    struct wl_client *client = wl_resource_get_client(seat->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat->pointers) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        wl_pointer_send_leave(resource, next_serial(resource),
                              seat->input_focus->surface->resource);
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
    struct server_seat *seat = data;

    // Update the pressed keys array.
    switch ((enum wl_keyboard_key_state)state) {
    case WL_KEYBOARD_KEY_STATE_PRESSED:
        for (size_t i = 0; i < seat->kb_state.pressed.len; i++) {
            if (seat->kb_state.pressed.data[i] == key) {
                ww_log(LOG_WARN, "duplicate key press event received");
                return;
            }
        }

        if (seat->kb_state.pressed.len == seat->kb_state.pressed.cap) {
            uint32_t *new_data = realloc(seat->kb_state.pressed.data,
                                         sizeof(uint32_t) * seat->kb_state.pressed.cap * 2);
            if (!new_data) {
                ww_log(LOG_WARN, "failed to reallocate pressed keys buffer - input dropped");
                return;
            }

            seat->kb_state.pressed.data = new_data;
            seat->kb_state.pressed.cap *= 2;
        }

        seat->kb_state.pressed.data[seat->kb_state.pressed.len++] = key;
        break;
    case WL_KEYBOARD_KEY_STATE_RELEASED:
        for (size_t i = 0; i < seat->kb_state.pressed.len; i++) {
            if (seat->kb_state.pressed.data[i] != key) {
                continue;
            }

            memmove(seat->kb_state.pressed.data + i, seat->kb_state.pressed.data + i + 1,
                    sizeof(uint32_t) * (seat->kb_state.pressed.len - i - 1));
            seat->kb_state.pressed.len--;
            break;
        }
        break;
    }

    if (seat->listener) {
        // Only send a key event to the wall code if this keycode has an associated keysym.
        // TODO: Probably not worth it to send all keysyms?

        const xkb_keysym_t *syms;

        // We need to add 8 to the keycode to convert from libinput to XKB keycodes. See
        // WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1.
        int nsyms = xkb_keymap_key_get_syms_by_level(seat->kb_state.remote_keymap.xkb, key + 8,
                                                     seat->kb_state.mods.group, 0, &syms);

        if (nsyms >= 1) {
            bool consumed = seat->listener->key(seat->listener_data, syms[0],
                                                state == WL_KEYBOARD_KEY_STATE_PRESSED);
            if (consumed) {
                return;
            }
        }
    }
    send_keyboard_key(seat, key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
}

static void
on_keyboard_keymap(void *data, struct wl_keyboard *wl, uint32_t format, int32_t fd, uint32_t size) {
    struct server_seat *seat = data;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        ww_log(LOG_WARN, "received keymap of unknown type %" PRIu32, format);
        return;
    }

    // TODO: handle errors somehow (not sure how to be honest)

    if (seat->kb_state.remote_keymap.fd >= 0) {
        close(seat->kb_state.remote_keymap.fd);
    }
    if (seat->kb_state.remote_keymap.xkb_state) {
        xkb_state_unref(seat->kb_state.remote_keymap.xkb_state);
        seat->kb_state.remote_keymap.xkb_state = NULL;
    }
    if (seat->kb_state.remote_keymap.xkb) {
        xkb_keymap_unref(seat->kb_state.remote_keymap.xkb);
    }
    seat->kb_state.remote_keymap.fd = fd;
    seat->kb_state.remote_keymap.size = size;

    seat->kb_state.remote_keymap.xkb = prepare_remote_keymap(seat->cfg->input.xkb_ctx, fd, size);
    if (!seat->kb_state.remote_keymap.xkb) {
        ww_log(LOG_ERROR, "failed to create remote keymap");
        return;
    }

    seat->kb_state.remote_keymap.xkb_state = xkb_state_new(seat->kb_state.remote_keymap.xkb);
    ww_assert(seat->kb_state.remote_keymap.xkb_state);

    for (size_t i = 0; i < STATIC_ARRLEN(seat->kb_state.remote_keymap.mods.indices); i++) {
        size_t mod = xkb_keymap_mod_get_index(seat->kb_state.remote_keymap.xkb, mod_names[i]);
        ww_assert(mod != XKB_MOD_INVALID);

        seat->kb_state.remote_keymap.mods.indices[i] = mod;
    }
}

static void
on_keyboard_leave(void *data, struct wl_keyboard *wl, uint32_t serial, struct wl_surface *surface) {
    struct server_seat *seat = data;

    for (size_t i = 0; i < seat->kb_state.pressed.len; i++) {
        send_keyboard_key(seat, seat->kb_state.pressed.data[i], WL_KEYBOARD_KEY_STATE_RELEASED);
    }
    seat->kb_state.pressed.len = 0;

    seat->kb_state.mods.depressed = 0;
    seat->kb_state.mods.latched = 0;
    seat->kb_state.mods.locked = 0;
    seat->kb_state.mods.group = 0;
    send_keyboard_modifiers(seat);
}

static void
on_keyboard_modifiers(void *data, struct wl_keyboard *wl, uint32_t serial, uint32_t mods_depressed,
                      uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
    struct server_seat *seat = data;

    // TODO: I don't think it's valid to just send the same modifiers here if there are two
    // different keymaps at play.

    seat->kb_state.mods.depressed = mods_depressed;
    seat->kb_state.mods.latched = mods_latched;
    seat->kb_state.mods.locked = mods_locked;
    seat->kb_state.mods.group = group;
    send_keyboard_modifiers(seat);

    if (seat->listener) {
        if (!seat->kb_state.remote_keymap.xkb_state) {
            return;
        }

        xkb_state_update_mask(seat->kb_state.remote_keymap.xkb_state, mods_depressed, mods_latched,
                              mods_locked, 0, 0, group);

        xkb_mod_mask_t xkb_mods = xkb_state_serialize_mods(seat->kb_state.remote_keymap.xkb_state,
                                                           XKB_STATE_MODS_EFFECTIVE);
        xkb_layout_index_t group = xkb_state_serialize_layout(
            seat->kb_state.remote_keymap.xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);

        uint32_t mods = 0;
        for (size_t i = 0; i < STATIC_ARRLEN(seat->kb_state.remote_keymap.mods.indices); i++) {
            uint8_t index = seat->kb_state.remote_keymap.mods.indices[i];
            if (xkb_mods & (1 << index)) {
                mods |= (1 << i);
            }
        }

        seat->listener->modifiers(seat->listener_data, mods, group);
    }
}

static void
on_keyboard_repeat_info(void *data, struct wl_keyboard *wl, int32_t rate, int32_t delay) {
    struct server_seat *seat = data;

    seat->kb_state.repeat_rate = rate;
    seat->kb_state.repeat_delay = delay;
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
    struct server_seat *seat = data;

    if (!seat->input_focus) {
        return;
    }

    struct wl_client *client = wl_resource_get_client(seat->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat->pointers) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        wl_pointer_send_axis(resource, current_time(), axis, value);
    }
}

static void
on_pointer_axis_discrete(void *data, struct wl_pointer *wl, uint32_t axis, int32_t discrete) {
    struct server_seat *seat = data;

    if (!seat->input_focus) {
        return;
    }

    struct wl_client *client = wl_resource_get_client(seat->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat->pointers) {
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
    struct server_seat *seat = data;

    if (!seat->input_focus) {
        return;
    }

    struct wl_client *client = wl_resource_get_client(seat->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat->pointers) {
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
    struct server_seat *seat = data;

    if (!seat->input_focus) {
        return;
    }

    struct wl_client *client = wl_resource_get_client(seat->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat->pointers) {
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
    struct server_seat *seat = data;

    if (seat->listener) {
        bool consumed = seat->listener->button(seat->listener_data, button,
                                               state == WL_POINTER_BUTTON_STATE_PRESSED);
        if (consumed) {
            return;
        }
    }

    if (!seat->input_focus) {
        return;
    }

    struct wl_client *client = wl_resource_get_client(seat->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat->pointers) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        wl_pointer_send_button(resource, next_serial(resource), current_time(), button, state);
    }
}

static void
on_pointer_enter(void *data, struct wl_pointer *wl, uint32_t serial, struct wl_surface *surface,
                 wl_fixed_t surface_x, wl_fixed_t surface_y) {
    struct server_seat *seat = data;

    if (surface != seat->server->ui.surface) {
        ww_log(LOG_WARN, "received wl_pointer.enter for unknown surface");
        return;
    }

    wl_signal_emit_mutable(&seat->events.pointer_enter, &serial);
}

static void
on_pointer_frame(void *data, struct wl_pointer *wl) {
    struct server_seat *seat = data;

    if (!seat->input_focus) {
        return;
    }

    struct wl_client *client = wl_resource_get_client(seat->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat->pointers) {
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
    struct server_seat *seat = data;

    seat->ptr_state.x = wl_fixed_to_double(surface_x);
    seat->ptr_state.y = wl_fixed_to_double(surface_y);

    if (seat->listener) {
        seat->listener->motion(seat->listener_data, seat->ptr_state.x, seat->ptr_state.y);
    }

    if (!seat->input_focus) {
        return;
    }

    double x, y;
    get_pointer_offset(seat, &x, &y);

    struct wl_client *client = wl_resource_get_client(seat->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat->pointers) {
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
on_input_focus(struct wl_listener *listener, void *data) {
    struct server_seat *seat = wl_container_of(listener, seat, on_input_focus);
    struct server_view *view = data;

    send_keyboard_leave(seat);
    send_pointer_leave(seat);
    seat->input_focus = view;
    if (seat->input_focus) {
        send_keyboard_enter(seat);
        send_pointer_enter(seat);
    }
}

static void
on_keyboard(struct wl_listener *listener, void *data) {
    struct server_seat *seat = wl_container_of(listener, seat, on_keyboard);

    seat->keyboard = server_get_wl_keyboard(seat->server);
    if (seat->keyboard) {
        wl_keyboard_add_listener(seat->keyboard, &keyboard_listener, seat);
    }
}

static void
on_pointer(struct wl_listener *listener, void *data) {
    struct server_seat *seat = wl_container_of(listener, seat, on_pointer);

    seat->pointer = server_get_wl_pointer(seat->server);
    if (seat->pointer) {
        wl_pointer_add_listener(seat->pointer, &pointer_listener, seat);
    }
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
    if (!surface_resource) {
        return;
    }

    // We do not care about what clients want to set the cursor to. However, we will mark
    // surfaces with the correct roles anyway.
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
    struct server_seat *seat = wl_resource_get_user_data(resource);

    struct wl_resource *keyboard_resource =
        wl_resource_create(client, &wl_keyboard_interface, wl_resource_get_version(resource), id);
    if (!keyboard_resource) {
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(keyboard_resource, &keyboard_impl, seat,
                                   keyboard_resource_destroy);

    wl_list_insert(&seat->keyboards, wl_resource_get_link(keyboard_resource));

    if (seat->cfg->input.custom_keymap) {
        wl_keyboard_send_keymap(keyboard_resource, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                seat->kb_state.local_keymap.fd, seat->kb_state.local_keymap.size);
    } else {
        wl_keyboard_send_keymap(keyboard_resource, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                seat->kb_state.remote_keymap.fd, seat->kb_state.remote_keymap.size);
    }

    int32_t rate = seat->cfg->input.repeat_rate >= 0 ? seat->cfg->input.repeat_rate
                                                     : seat->kb_state.repeat_rate;
    int32_t delay = seat->cfg->input.repeat_delay >= 0 ? seat->cfg->input.repeat_delay
                                                       : seat->kb_state.repeat_delay;
    wl_keyboard_send_repeat_info(keyboard_resource, rate, delay);
}

static void
seat_get_pointer(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_seat *seat = wl_resource_get_user_data(resource);

    struct wl_resource *pointer_resource =
        wl_resource_create(client, &wl_pointer_interface, wl_resource_get_version(resource), id);
    if (!pointer_resource) {
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(pointer_resource, &pointer_impl, seat, pointer_resource_destroy);

    wl_list_insert(&seat->pointers, wl_resource_get_link(pointer_resource));
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

    struct server_seat *seat = data;

    struct wl_resource *resource = wl_resource_create(client, &wl_seat_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &seat_impl, seat, seat_resource_destroy);

    if (version >= WL_SEAT_NAME_SINCE_VERSION) {
        wl_seat_send_name(resource, "Waywall Seat");
    }
    wl_seat_send_capabilities(resource, WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_POINTER);
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_seat *seat = wl_container_of(listener, seat, on_display_destroy);

    wl_global_destroy(seat->global);

    if (seat->kb_state.local_keymap.fd >= 0) {
        close(seat->kb_state.local_keymap.fd);
    }
    if (seat->kb_state.remote_keymap.fd >= 0) {
        close(seat->kb_state.remote_keymap.fd);
    }
    if (seat->kb_state.remote_keymap.xkb_state) {
        xkb_state_unref(seat->kb_state.remote_keymap.xkb_state);
    }
    if (seat->kb_state.remote_keymap.xkb) {
        xkb_keymap_unref(seat->kb_state.remote_keymap.xkb);
    }
    if (seat->kb_state.pressed.data) {
        free(seat->kb_state.pressed.data);
    }

    wl_list_remove(&seat->on_input_focus.link);
    wl_list_remove(&seat->on_keyboard.link);
    wl_list_remove(&seat->on_pointer.link);

    wl_list_remove(&seat->on_display_destroy.link);

    free(seat);
}

struct server_seat *
server_seat_create(struct server *server, struct config *cfg) {
    struct server_seat *seat = calloc(1, sizeof(*seat));
    if (!seat) {
        ww_log(LOG_ERROR, "failed to allocate server_seat");
        return NULL;
    }

    seat->cfg = cfg;
    seat->server = server;

    seat->kb_state.local_keymap.fd = -1;
    if (prepare_keymap(seat) != 0) {
        goto fail_keymap;
    }

    seat->global = wl_global_create(server->display, &wl_seat_interface, SRV_SEAT_VERSION, seat,
                                    on_global_bind);
    if (!seat->global) {
        ww_log(LOG_ERROR, "failed to allocate seat global");
        goto fail_global;
    }

    seat->kb_state.remote_keymap.fd = -1;
    seat->kb_state.pressed.cap = 8;
    seat->kb_state.pressed.data = calloc(seat->kb_state.pressed.cap, sizeof(uint32_t));
    if (!seat->kb_state.pressed.data) {
        ww_log(LOG_ERROR, "failed to allocate pressed keys array");
        goto fail_pressed_keys;
    }

    wl_list_init(&seat->keyboards);
    wl_list_init(&seat->pointers);

    wl_signal_init(&seat->events.pointer_enter);

    seat->on_input_focus.notify = on_input_focus;
    wl_signal_add(&server->events.input_focus, &seat->on_input_focus);

    seat->on_keyboard.notify = on_keyboard;
    wl_signal_add(&server->backend.events.seat_keyboard, &seat->on_keyboard);

    seat->on_pointer.notify = on_pointer;
    wl_signal_add(&server->backend.events.seat_pointer, &seat->on_pointer);

    seat->keyboard = server_get_wl_keyboard(server);
    if (seat->keyboard) {
        wl_keyboard_add_listener(seat->keyboard, &keyboard_listener, seat);
    }

    seat->pointer = server_get_wl_pointer(server);
    if (seat->pointer) {
        wl_pointer_add_listener(seat->pointer, &pointer_listener, seat);
    }

    seat->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &seat->on_display_destroy);

    return seat;

fail_pressed_keys:
    wl_global_destroy(seat->global);

fail_global:
    close(seat->kb_state.local_keymap.fd);

fail_keymap:
    free(seat);
    return NULL;
}

void
server_seat_send_click(struct server_seat *seat, struct server_view *view) {
    ww_assert(seat->input_focus != view);

    uint32_t time = current_time();

    struct wl_client *client = wl_resource_get_client(view->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat->pointers) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        wl_pointer_send_enter(resource, next_serial(resource), view->surface->resource, 0, 0);
        wl_pointer_send_button(resource, next_serial(resource), time, BTN_LEFT,
                               WL_POINTER_BUTTON_STATE_PRESSED);
        wl_pointer_send_button(resource, next_serial(resource), time, BTN_LEFT,
                               WL_POINTER_BUTTON_STATE_RELEASED);
        wl_pointer_send_leave(resource, next_serial(resource), view->surface->resource);
    }
}

void
server_seat_send_keys(struct server_seat *seat, struct server_view *view,
                      const struct syn_key *keys, size_t num_keys) {
    struct wl_client *client = wl_resource_get_client(view->surface->resource);
    struct wl_resource *resource;

    struct wl_array wl_keys;
    wl_array_init(&wl_keys);

    uint32_t time = current_time();

    wl_resource_for_each(resource, &seat->keyboards) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        if (seat->input_focus != view) {
            wl_keyboard_send_enter(resource, next_serial(resource), view->surface->resource,
                                   &wl_keys);
        }

        for (size_t i = 0; i < num_keys; i++) {
            wl_keyboard_send_key(resource, next_serial(resource), time, keys[i].keycode,
                                 keys[i].press ? WL_KEYBOARD_KEY_STATE_PRESSED
                                               : WL_KEYBOARD_KEY_STATE_RELEASED);
        }

        if (seat->input_focus != view) {
            wl_keyboard_send_leave(resource, next_serial(resource), view->surface->resource);
        }
    }

    wl_array_release(&wl_keys);
}

void
server_seat_set_listener(struct server_seat *seat, const struct server_seat_listener *listener,
                         void *data) {
    ww_assert(!seat->listener);

    seat->listener = listener;
    seat->listener_data = data;
}
