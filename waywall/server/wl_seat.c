#include "server/wl_seat.h"
#include "config/config.h"
#include "server/backend.h"
#include "server/server.h"
#include "server/ui.h"
#include "server/wl_compositor.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include "util/serial.h"
#include "util/str.h"
#include "util/syscall.h"
#include <inttypes.h>
#include <linux/input-event-codes.h>
#include <linux/memfd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon-names.h>
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

    *x = seat->pointer.x - (double)seat->input_focus->current.x;
    *y = seat->pointer.y - (double)seat->input_focus->current.y;
}

static int
prepare_local_keymap(struct server_seat *seat, const struct xkb_rule_names *rule_names,
                     struct server_seat_keymap *km) {
    km->xkb = xkb_keymap_new_from_names(seat->ctx, rule_names, XKB_MAP_COMPILE_NO_FLAGS);
    if (!km->xkb) {
        ww_log(LOG_ERROR, "failed to create XKB keymap");
        goto fail_keymap;
    }

    km->state = xkb_state_new(km->xkb);
    if (!km->state) {
        ww_log(LOG_ERROR, "failed to create XKB state");
        goto fail_state;
    }

    char *km_str = xkb_keymap_get_as_string(km->xkb, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!km_str) {
        ww_log(LOG_ERROR, "failed to get XKB keymap string");
        goto fail_keymap_string;
    }

    km->size = strlen(km_str) + 1;
    km->fd = memfd_create("waywall-keymap", MFD_CLOEXEC);
    if (km->fd == -1) {
        ww_log_errno(LOG_ERROR, "failed to create XKB keymap memfd");
        goto fail_create_memfd;
    }
    if (ftruncate(km->fd, km->size) == -1) {
        ww_log_errno(LOG_ERROR, "failed to expand XKB keymap memfd");
        goto fail_truncate_memfd;
    }

    char *data = mmap(NULL, km->size, PROT_READ | PROT_WRITE, MAP_SHARED, km->fd, 0);
    if (data == MAP_FAILED) {
        ww_log_errno(LOG_ERROR, "failed to map XKB keymap memfd");
        goto fail_map_memfd;
    }

    memcpy(data, km_str, km->size);
    ww_assert(munmap(data, km->size) == 0);
    free(km_str);

    return 0;

fail_map_memfd:
fail_truncate_memfd:
    close(km->fd);

fail_create_memfd:
    free(km_str);

fail_keymap_string:
    xkb_state_unref(km->state);

fail_state:
    xkb_keymap_unref(km->xkb);

fail_keymap:
    return 1;
}

static void
server_seat_keymap_destroy(struct server_seat_keymap *keymap) {
    if (keymap->fd >= 0) {
        close(keymap->fd);
    }
    if (keymap->state) {
        xkb_state_unref(keymap->state);
    }
    if (keymap->xkb) {
        xkb_keymap_unref(keymap->xkb);
    }
}

static void
xkb_log(struct xkb_context *ctx, enum xkb_log_level xkb_level, const char *fmt, va_list args) {
    enum ww_log_level level;
    switch (xkb_level) {
    case XKB_LOG_LEVEL_CRITICAL:
    case XKB_LOG_LEVEL_ERROR:
        level = LOG_ERROR;
        break;
    case XKB_LOG_LEVEL_WARNING:
        level = LOG_WARN;
        break;
    case XKB_LOG_LEVEL_INFO:
    case XKB_LOG_LEVEL_DEBUG:
        level = LOG_INFO;
        break;
    default:
        level = LOG_ERROR;
        break;
    }

    str logline = str_new();
    str_append(&logline, "[xkb] ");
    str_append(&logline, fmt);

    util_log_va(level, logline, args, false);
    str_free(logline);

    va_end(args);
}

static bool
modify_pressed_keys(struct server_seat *seat, uint32_t keycode, bool state) {
    if (state) {
        for (size_t i = 0; i < seat->keyboard.pressed.len; i++) {
            if (seat->keyboard.pressed.data[i] == keycode) {
                ww_log(LOG_WARN, "duplicate key press event received");
                return false;
            }
        }

        if (seat->keyboard.pressed.len == seat->keyboard.pressed.cap) {
            ww_assert(seat->keyboard.pressed.cap > 0);

            uint32_t *new_data = realloc(seat->keyboard.pressed.data,
                                         sizeof(uint32_t) * seat->keyboard.pressed.cap * 2);
            check_alloc(new_data);

            seat->keyboard.pressed.data = new_data;
            seat->keyboard.pressed.cap *= 2;
        }

        seat->keyboard.pressed.data[seat->keyboard.pressed.len++] = keycode;
        if (xkb_state_update_key(seat->config->keymap.state, keycode + 8, XKB_KEY_DOWN) != 0) {
            return true;
        }
    } else {
        bool found = false;

        for (size_t i = 0; i < seat->keyboard.pressed.len; i++) {
            if (seat->keyboard.pressed.data[i] != keycode) {
                continue;
            }

            memmove(seat->keyboard.pressed.data + i, seat->keyboard.pressed.data + i + 1,
                    sizeof(uint32_t) * (seat->keyboard.pressed.len - i - 1));
            seat->keyboard.pressed.len--;
            found = true;
            break;
        }

        if (found) {
            if (xkb_state_update_key(seat->config->keymap.state, keycode + 8, XKB_KEY_UP) != 0) {
                return true;
            }
        }
    }
    return false;
}

static void
send_keyboard_enter(struct server_seat *seat) {
    ww_assert(seat->input_focus);

    struct wl_array keys;
    wl_array_init(&keys);
    uint32_t *data = wl_array_add(&keys, sizeof(uint32_t) * seat->keyboard.pressed.len);
    check_alloc(data);
    for (size_t i = 0; i < seat->keyboard.pressed.len; i++) {
        data[i] = seat->keyboard.pressed.data[i];
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

    xkb_mod_mask_t depressed =
        xkb_state_serialize_mods(seat->config->keymap.state, XKB_STATE_MODS_DEPRESSED);
    xkb_mod_mask_t latched =
        xkb_state_serialize_mods(seat->config->keymap.state, XKB_STATE_MODS_LATCHED);
    xkb_mod_mask_t locked =
        xkb_state_serialize_mods(seat->config->keymap.state, XKB_STATE_MODS_LOCKED);
    xkb_layout_index_t group =
        xkb_state_serialize_layout(seat->config->keymap.state, XKB_STATE_LAYOUT_EFFECTIVE);

    struct wl_client *client = wl_resource_get_client(seat->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &seat->keyboards) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        wl_keyboard_send_modifiers(resource, next_serial(resource), depressed, latched, locked,
                                   group);
    }
}

static void
send_pointer_button(struct server_seat *seat, uint32_t button, bool state) {
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
reset_keyboard_state(struct server_seat *seat) {
    for (size_t i = 0; i < seat->keyboard.pressed.len; i++) {
        xkb_state_update_key(seat->config->keymap.state, seat->keyboard.pressed.data[i] + 8,
                             XKB_KEY_UP);
        send_keyboard_key(seat, seat->keyboard.pressed.data[i], WL_KEYBOARD_KEY_STATE_RELEASED);
    }
    seat->keyboard.pressed.len = 0;
}

static void
use_local_keymap(struct server_seat *seat, struct server_seat_keymap keymap) {
    seat->config->keymap = keymap;

    reset_keyboard_state(seat);

    struct wl_resource *keyboard_resource;
    wl_resource_for_each(keyboard_resource, &seat->keyboards) {
        wl_keyboard_send_keymap(keyboard_resource, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, keymap.fd,
                                keymap.size);
    }
}

static void
process_remap_key(struct server_seat *seat, uint32_t keycode, bool state) {
    bool modifiers_updated = modify_pressed_keys(seat, keycode, state);

    if (modifiers_updated) {
        send_keyboard_modifiers(seat);
    }
    send_keyboard_key(seat, keycode, state);
}

static void
process_remap(struct server_seat *seat, struct server_seat_remap remap, bool state) {
    switch (remap.type) {
    case CONFIG_REMAP_BUTTON:
        send_pointer_button(seat, remap.dst, state);
        return;
    case CONFIG_REMAP_KEY:
        process_remap_key(seat, remap.dst, state);
        return;
    case CONFIG_REMAP_NONE:
        ww_unreachable();
    }
}

static bool
try_remap_button(struct server_seat *seat, uint32_t button, bool state) {
    for (size_t i = 0; i < seat->config->remaps.num_buttons; i++) {
        if (seat->config->remaps.buttons[i].src == button) {
            process_remap(seat, seat->config->remaps.buttons[i], state);
            return true;
        }
    }
    return false;
}

static bool
try_remap_key(struct server_seat *seat, uint32_t keycode, bool state) {
    for (size_t i = 0; i < seat->config->remaps.num_keys; i++) {
        if (seat->config->remaps.keys[i].src == keycode) {
            if (seat->config->remaps.buttons[i].src == keycode) {
                process_remap(seat, seat->config->remaps.buttons[i], state);
                return true;
            }
        }
    }
    return false;
}

static void
on_keyboard_enter(void *data, struct wl_keyboard *wl, uint32_t serial, struct wl_surface *surface,
                  struct wl_array *keys) {
    struct server_seat *seat = data;
    seat->last_serial = serial;

    wl_signal_emit_mutable(&seat->events.keyboard_enter, &serial);
}

static void
on_keyboard_key(void *data, struct wl_keyboard *wl, uint32_t serial, uint32_t time, uint32_t key,
                uint32_t state) {
    struct server_seat *seat = data;
    seat->last_serial = serial;

    if (try_remap_key(seat, key, state == WL_KEYBOARD_KEY_STATE_PRESSED)) {
        return;
    }

    bool modifiers_updated = modify_pressed_keys(seat, key, state == WL_KEYBOARD_KEY_STATE_PRESSED);

    if (seat->listener) {
        const xkb_keysym_t *syms;

        xkb_layout_index_t group =
            xkb_state_serialize_layout(seat->keyboard.remote_km.state, XKB_STATE_LAYOUT_EFFECTIVE);

        // We need to add 8 to the keycode to convert from libinput to XKB keycodes. See
        // WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1.
        int nsyms = xkb_keymap_key_get_syms_by_level(seat->keyboard.remote_km.xkb, key + 8, group,
                                                     0, &syms);

        if (nsyms >= 1) {
            bool consumed = seat->listener->key(seat->listener_data, syms[0],
                                                state == WL_KEYBOARD_KEY_STATE_PRESSED);
            if (consumed) {
                return;
            }
        }
    }

    if (modifiers_updated) {
        send_keyboard_modifiers(seat);
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

    // Prepare new keymap.
    char *km_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (km_str == MAP_FAILED) {
        ww_log_errno(LOG_ERROR, "failed to mmap keymap data");
        return;
    }

    struct xkb_keymap *keymap = xkb_keymap_new_from_string(
        seat->ctx, km_str, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_MAP_COMPILE_NO_FLAGS);
    ww_assert(munmap(km_str, size) == 0);
    if (!keymap) {
        return;
    }

    // Release resources associated with the previous keymap, if any.
    if (seat->keyboard.remote_km.fd >= 0) {
        close(seat->keyboard.remote_km.fd);
    }
    if (seat->keyboard.remote_km.state) {
        xkb_state_unref(seat->keyboard.remote_km.state);
    }
    if (seat->keyboard.remote_km.xkb) {
        xkb_keymap_unref(seat->keyboard.remote_km.xkb);
    }

    // Apply the new keymap.
    seat->keyboard.remote_km.xkb = keymap;
    seat->keyboard.remote_km.fd = fd;
    seat->keyboard.remote_km.size = size;

    seat->keyboard.remote_km.state = xkb_state_new(seat->keyboard.remote_km.xkb);
    check_alloc(seat->keyboard.remote_km.state);

    for (size_t i = 0; i < STATIC_ARRLEN(seat->keyboard.mod_indices); i++) {
        size_t mod = xkb_keymap_mod_get_index(seat->keyboard.remote_km.xkb, mod_names[i]);
        ww_assert(mod != XKB_MOD_INVALID);

        seat->keyboard.mod_indices[i] = mod;
    }
}

static void
on_keyboard_leave(void *data, struct wl_keyboard *wl, uint32_t serial, struct wl_surface *surface) {
    struct server_seat *seat = data;
    seat->last_serial = serial;

    reset_keyboard_state(seat);

    wl_signal_emit_mutable(&seat->events.keyboard_leave, &serial);
}

static void
on_keyboard_modifiers(void *data, struct wl_keyboard *wl, uint32_t serial, uint32_t mods_depressed,
                      uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
    struct server_seat *seat = data;
    seat->last_serial = serial;

    if (seat->listener) {
        if (!seat->keyboard.remote_km.state) {
            return;
        }

        xkb_state_update_mask(seat->keyboard.remote_km.state, mods_depressed, mods_latched,
                              mods_locked, 0, 0, group);

        xkb_mod_mask_t xkb_mods =
            xkb_state_serialize_mods(seat->keyboard.remote_km.state, XKB_STATE_MODS_EFFECTIVE);

        uint32_t mods = 0;
        for (size_t i = 0; i < STATIC_ARRLEN(seat->keyboard.mod_indices); i++) {
            uint8_t index = seat->keyboard.mod_indices[i];
            if (xkb_mods & (1 << index)) {
                mods |= (1 << i);
            }
        }

        seat->listener->modifiers(seat->listener_data, mods);
    }
}

static void
on_keyboard_repeat_info(void *data, struct wl_keyboard *wl, int32_t rate, int32_t delay) {
    struct server_seat *seat = data;

    seat->keyboard.repeat_rate = rate;
    seat->keyboard.repeat_delay = delay;
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
    seat->last_serial = serial;

    if (try_remap_button(seat, button, state == WL_POINTER_BUTTON_STATE_PRESSED)) {
        return;
    }

    if (seat->listener) {
        bool consumed = seat->listener->button(seat->listener_data, button,
                                               state == WL_POINTER_BUTTON_STATE_PRESSED);
        if (consumed) {
            return;
        }
    }

    send_pointer_button(seat, button, state == WL_POINTER_BUTTON_STATE_PRESSED);
}

static void
on_pointer_enter(void *data, struct wl_pointer *wl, uint32_t serial, struct wl_surface *surface,
                 wl_fixed_t surface_x, wl_fixed_t surface_y) {
    struct server_seat *seat = data;
    seat->last_serial = serial;

    if (surface != seat->server->ui->root.surface) {
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
    struct server_seat *seat = data;
    seat->last_serial = serial;
}

static void
on_pointer_motion(void *data, struct wl_pointer *wl, uint32_t time, wl_fixed_t surface_x,
                  wl_fixed_t surface_y) {
    struct server_seat *seat = data;

    seat->pointer.x = wl_fixed_to_double(surface_x);
    seat->pointer.y = wl_fixed_to_double(surface_y);

    if (seat->listener) {
        seat->listener->motion(seat->listener_data, seat->pointer.x, seat->pointer.y);
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

    seat->keyboard.remote = server_get_wl_keyboard(seat->server);
    if (seat->keyboard.remote) {
        wl_keyboard_add_listener(seat->keyboard.remote, &keyboard_listener, seat);
    }
}

static void
on_pointer(struct wl_listener *listener, void *data) {
    struct server_seat *seat = wl_container_of(listener, seat, on_pointer);

    seat->pointer.remote = server_get_wl_pointer(seat->server);
    if (seat->pointer.remote) {
        wl_pointer_add_listener(seat->pointer.remote, &pointer_listener, seat);
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
    check_alloc(keyboard_resource);
    wl_resource_set_implementation(keyboard_resource, &keyboard_impl, seat,
                                   keyboard_resource_destroy);

    wl_list_insert(&seat->keyboards, wl_resource_get_link(keyboard_resource));

    wl_keyboard_send_keymap(keyboard_resource, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                            seat->config->keymap.fd, seat->config->keymap.size);

    int32_t rate =
        seat->config->repeat_rate >= 0 ? seat->config->repeat_rate : seat->keyboard.repeat_rate;
    int32_t delay =
        seat->config->repeat_delay >= 0 ? seat->config->repeat_delay : seat->keyboard.repeat_delay;
    wl_keyboard_send_repeat_info(keyboard_resource, rate, delay);
}

static void
seat_get_pointer(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_seat *seat = wl_resource_get_user_data(resource);

    struct wl_resource *pointer_resource =
        wl_resource_create(client, &wl_pointer_interface, wl_resource_get_version(resource), id);
    check_alloc(pointer_resource);
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
    check_alloc(resource);
    wl_resource_set_implementation(resource, &seat_impl, seat, seat_resource_destroy);

    if (version >= WL_SEAT_NAME_SINCE_VERSION) {
        wl_seat_send_name(resource, "Waywall Seat");
    }
    wl_seat_send_capabilities(resource, WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_POINTER);
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_seat *seat = wl_container_of(listener, seat, on_display_destroy);

    server_seat_config_destroy(seat->config);

    wl_global_destroy(seat->global);

    server_seat_keymap_destroy(&seat->keyboard.remote_km);
    free(seat->keyboard.pressed.data);

    xkb_context_unref(seat->ctx);

    wl_list_remove(&seat->on_input_focus.link);
    wl_list_remove(&seat->on_keyboard.link);
    wl_list_remove(&seat->on_pointer.link);

    wl_list_remove(&seat->on_display_destroy.link);

    free(seat);
}

struct server_seat *
server_seat_create(struct server *server, struct config *cfg) {
    struct server_seat *seat = zalloc(1, sizeof(*seat));

    seat->server = server;

    seat->ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!seat->ctx) {
        ww_log(LOG_ERROR, "failed to create xkb_context");
        goto fail_xkb_context;
    }
    xkb_context_set_log_fn(seat->ctx, xkb_log);

    seat->global = wl_global_create(server->display, &wl_seat_interface, SRV_SEAT_VERSION, seat,
                                    on_global_bind);
    check_alloc(seat->global);

    seat->keyboard.remote_km.fd = -1;
    seat->keyboard.pressed.cap = 8;
    seat->keyboard.pressed.data = zalloc(seat->keyboard.pressed.cap, sizeof(uint32_t));

    wl_list_init(&seat->keyboards);
    wl_list_init(&seat->pointers);

    wl_signal_init(&seat->events.keyboard_enter);
    wl_signal_init(&seat->events.keyboard_leave);
    wl_signal_init(&seat->events.pointer_enter);

    seat->on_input_focus.notify = on_input_focus;
    wl_signal_add(&server->events.input_focus, &seat->on_input_focus);

    seat->on_keyboard.notify = on_keyboard;
    wl_signal_add(&server->backend->events.seat_keyboard, &seat->on_keyboard);

    seat->on_pointer.notify = on_pointer;
    wl_signal_add(&server->backend->events.seat_pointer, &seat->on_pointer);

    seat->keyboard.remote = server_get_wl_keyboard(server);
    if (seat->keyboard.remote) {
        wl_keyboard_add_listener(seat->keyboard.remote, &keyboard_listener, seat);
    }

    seat->pointer.remote = server_get_wl_pointer(server);
    if (seat->pointer.remote) {
        wl_pointer_add_listener(seat->pointer.remote, &pointer_listener, seat);
    }

    seat->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &seat->on_display_destroy);

    struct server_seat_config *config = server_seat_config_create(seat, cfg);
    if (!config) {
        ww_log(LOG_ERROR, "failed to create server seat config");
        goto fail_config;
    }
    server_seat_use_config(seat, config);

    return seat;

fail_config:
    wl_list_remove(&seat->on_display_destroy.link);
    wl_list_remove(&seat->on_pointer.link);
    wl_list_remove(&seat->on_keyboard.link);
    wl_list_remove(&seat->on_input_focus.link);
    free(seat->keyboard.pressed.data);
    xkb_context_unref(seat->ctx);

fail_xkb_context:
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
server_seat_send_keys(struct server_seat *seat, struct server_view *view, size_t num_keys,
                      const struct syn_key keys[static num_keys]) {
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

void
server_seat_use_config(struct server_seat *seat, struct server_seat_config *config) {
    if (seat->config) {
        server_seat_config_destroy(seat->config);
    }
    seat->config = config;

    use_local_keymap(seat, seat->config->keymap);
}

struct server_seat_config *
server_seat_config_create(struct server_seat *seat, struct config *cfg) {
    struct server_seat_config *config = zalloc(1, sizeof(*config));

    config->repeat_rate = cfg->input.repeat_rate;
    config->repeat_delay = cfg->input.repeat_delay;

    const struct xkb_rule_names rule_names = {
        .layout = cfg->input.keymap.layout,
        .model = cfg->input.keymap.model,
        .rules = cfg->input.keymap.rules,
        .variant = cfg->input.keymap.variant,
        .options = cfg->input.keymap.options,
    };

    if (prepare_local_keymap(seat, &rule_names, &config->keymap) != 0) {
        goto fail_keymap;
    }

    // It's not worth the effort to calculate how many of each kind of remap there are. The number
    // of remaps a user might reasonably have is quite small.
    config->remaps.keys = zalloc(cfg->input.remaps.count, sizeof(*config->remaps.keys));
    config->remaps.buttons = zalloc(cfg->input.remaps.count, sizeof(*config->remaps.buttons));

    for (size_t i = 0; i < cfg->input.remaps.count; i++) {
        struct config_remap *remap = &cfg->input.remaps.data[i];

        struct server_seat_remap *dst = NULL;
        switch (remap->src_type) {
        case CONFIG_REMAP_BUTTON:
            dst = &config->remaps.buttons[config->remaps.num_buttons++];
            break;
        case CONFIG_REMAP_KEY:
            dst = &config->remaps.keys[config->remaps.num_keys++];
            break;
        default:
            ww_unreachable();
        }

        dst->dst = remap->dst_data;
        dst->src = remap->src_data;
        dst->type = remap->dst_type;
    }

    return config;

fail_keymap:
    free(config);
    return NULL;
}

void
server_seat_config_destroy(struct server_seat_config *config) {
    free(config->remaps.keys);
    free(config->remaps.buttons);
    server_seat_keymap_destroy(&config->keymap);
    free(config);
}

int
server_seat_lua_set_keymap(struct server_seat *seat, const struct xkb_rule_names *rule_names) {
    struct server_seat_keymap keymap = {0};

    if (prepare_local_keymap(seat, rule_names, &keymap) != 0) {
        return 1;
    }

    server_seat_keymap_destroy(&seat->config->keymap);
    use_local_keymap(seat, keymap);

    return 0;
}
