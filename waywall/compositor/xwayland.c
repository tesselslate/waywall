/*
 *  The xwayland module is responsible for maintaining the Xwayland state. It is not involved in the
 *  presentation of windows on outputs, instead delegating that to other modules (e.g. compositor
 *  and hview) through the use of signals.
 */

#include "compositor/xwayland.h"
#include "compositor/input.h"
#include "util.h"
#include <time.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include <xcb/xcb.h>

// HACK: Any reason xwm_destroy isn't called already by wlr_xwayland_destroy? Maybe ask wlroots
// people about this.
extern void xwm_destroy(struct wlr_xwm *xwm);

static uint32_t
now_msec() {
    // HACK: For now Xwayland uses CLOCK_MONOTONIC and CLOCK_MONOTONIC uses some point near system
    // boot as its epoch. Hopefully this remains the case forever, since I don't want to replicate
    // the awful time calculation logic from resetti.

    // HACK: GLFW expects each keypress to have an ascending timestamp. We must make sure each
    // timestamp returned by this function is greater than the last.

    // TODO: If there ever exists any case where we issue two consecutive keypresses for the same
    // key, we must increment the timestamp by 20ms. This will need to track additional state at
    // that point.

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
send_event(struct xwl_window *window, uint32_t mask, const char *event) {
    xcb_void_cookie_t cookie =
        xcb_send_event_checked(window->xwl->xcb, true, window->surface->window_id, mask, event);
    xcb_generic_error_t *err = xcb_request_check(window->xwl->xcb, cookie);
    if (err) {
        int opcode = (int)(event[0]);
        wlr_log(WLR_ERROR, "failed to send event (opcode: %d, window: %d): %d", opcode, window->surface->window_id, err->error_code);
        free(err);
    }
    return err == NULL;
}

static void
handle_surface_map(struct wl_listener *listener, void *data) {
    struct xwl_window *window = wl_container_of(listener, window, on_map);

    wlr_log(WLR_DEBUG, "window %" PRIu32 " mapped", window->surface->window_id);

    window->mapped = true;
    wl_list_insert(&window->xwl->windows, &window->link);

    wl_signal_emit_mutable(&window->events.map, window);
    wl_signal_emit_mutable(&window->xwl->events.window_map, window);
}

static void
handle_surface_unmap(struct wl_listener *listener, void *data) {
    struct xwl_window *window = wl_container_of(listener, window, on_unmap);

    wlr_log(WLR_DEBUG, "window %" PRIu32 " unmapped", window->surface->window_id);

    window->mapped = false;
    wl_list_remove(&window->link);

    wl_signal_emit_mutable(&window->events.unmap, window);
    wl_signal_emit_mutable(&window->xwl->events.window_unmap, window);
}

static void
handle_surface_associate(struct wl_listener *listener, void *data) {
    struct xwl_window *window = wl_container_of(listener, window, on_associate);

    window->on_map.notify = handle_surface_map;
    wl_signal_add(&window->surface->surface->events.map, &window->on_map);

    window->on_unmap.notify = handle_surface_unmap;
    wl_signal_add(&window->surface->surface->events.unmap, &window->on_unmap);
}

static void
handle_surface_dissociate(struct wl_listener *listener, void *data) {
    struct xwl_window *window = wl_container_of(listener, window, on_dissociate);

    wl_list_remove(&window->on_map.link);
    wl_list_remove(&window->on_unmap.link);
}

static void
handle_surface_destroy(struct wl_listener *listener, void *data) {
    struct xwl_window *window = wl_container_of(listener, window, on_destroy);

    wl_signal_emit_mutable(&window->events.destroy, window);
    wl_signal_emit_mutable(&window->xwl->events.window_destroy, window);

    wl_list_remove(&window->on_associate.link);
    wl_list_remove(&window->on_dissociate.link);
    wl_list_remove(&window->on_destroy.link);
    wl_list_remove(&window->on_request_activate.link);
    wl_list_remove(&window->on_request_configure.link);
    wl_list_remove(&window->on_request_fullscreen.link);

    free(window);
}

static void
handle_surface_request_activate(struct wl_listener *listener, void *data) {
    struct xwl_window *window = wl_container_of(listener, window, on_request_activate);

    wlr_log(WLR_DEBUG, "window %" PRIu32 " requested activation", window->surface->window_id);
}

static void
handle_surface_request_configure(struct wl_listener *listener, void *data) {
    struct xwl_window *window = wl_container_of(listener, window, on_request_configure);
    struct wlr_xwayland_surface_configure_event *event = data;

    wlr_log(WLR_DEBUG,
            "window %" PRIu32 " requested configuration (%" PRIi16 " x %" PRIi16 " + %" PRIu16
            ", %" PRIu16 ")",
            window->surface->window_id, event->x, event->y, event->width, event->height);

    if (!window->mapped || window->floating) {
        wlr_xwayland_surface_configure(window->surface, event->x, event->y, event->width,
                                       event->height);
        wl_signal_emit_mutable(&window->events.configure, event);
        return;
    }
}

static void
handle_surface_request_fullscreen(struct wl_listener *listener, void *data) {
    struct xwl_window *window = wl_container_of(listener, window, on_request_fullscreen);

    wlr_log(WLR_DEBUG, "window %" PRIu32 " requested fullscreen (%d)", window->surface->window_id,
            window->surface->fullscreen);
}

static void
handle_surface_request_minimize(struct wl_listener *listener, void *data) {
    struct xwl_window *window = wl_container_of(listener, window, on_request_minimize);
    struct wlr_xwayland_minimize_event *event = data;

    wlr_log(WLR_DEBUG, "window %" PRIu32 " requested minimization (%d)", window->surface->window_id,
            event->minimize);
    wlr_xwayland_surface_set_minimized(window->surface, event->minimize);
    wl_signal_emit_mutable(&window->events.minimize, event);
}

static void
handle_new_surface(struct wl_listener *listener, void *data) {
    struct comp_xwayland *xwl = wl_container_of(listener, xwl, on_new_surface);
    struct wlr_xwayland_surface *surface = data;

    // waywall is not designed with override redirect clients in mind.
    if (surface->override_redirect) {
        wlr_log(WLR_INFO, "window %" PRIu32 " wants override redirect", surface->window_id);
        xcb_kill_client(xwl->xcb, surface->window_id);
        return;
    }

    struct xwl_window *window = calloc(1, sizeof(struct xwl_window));
    ww_assert(window);

    window->xwl = xwl;
    window->surface = surface;

    window->on_associate.notify = handle_surface_associate;
    wl_signal_add(&surface->events.associate, &window->on_associate);

    window->on_dissociate.notify = handle_surface_dissociate;
    wl_signal_add(&surface->events.dissociate, &window->on_dissociate);

    window->on_destroy.notify = handle_surface_destroy;
    wl_signal_add(&surface->events.destroy, &window->on_destroy);

    window->on_request_activate.notify = handle_surface_request_activate;
    wl_signal_add(&surface->events.request_activate, &window->on_request_activate);

    window->on_request_configure.notify = handle_surface_request_configure;
    wl_signal_add(&surface->events.request_configure, &window->on_request_configure);

    window->on_request_fullscreen.notify = handle_surface_request_fullscreen;
    wl_signal_add(&surface->events.request_fullscreen, &window->on_request_fullscreen);

    window->on_request_minimize.notify = handle_surface_request_minimize;
    wl_signal_add(&surface->events.request_minimize, &window->on_request_minimize);

    wl_signal_init(&window->events.map);
    wl_signal_init(&window->events.unmap);
    wl_signal_init(&window->events.configure);
    wl_signal_init(&window->events.minimize);
    wl_signal_init(&window->events.destroy);
}

static void
handle_ready(struct wl_listener *listener, void *data) {
    struct comp_xwayland *xwl = wl_container_of(listener, xwl, on_ready);

    xwl->xcb = xcb_connect(xwl->xwayland->display_name, NULL);
    int err = xcb_connection_has_error(xwl->xcb);
    if (err) {
        wlr_log(WLR_ERROR, "failed to connect to xwayland server: %d", err);
        wl_display_terminate(xwl->compositor->display);
    }
}

void
xwl_click(struct xwl_window *window) {
    ww_assert(window);

    // HACK: We send enter and leave notify events to get GLFW to update the cursor position.

    static const uint32_t button_mask = XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE;
    static const uint32_t notify_mask = XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW;

    xcb_enter_notify_event_t notify_event = {
        .response_type = XCB_ENTER_NOTIFY,
        .root = window->surface->window_id,
        .event = window->surface->window_id,
        .child = window->surface->window_id,
        0,
    };
    send_event(window, notify_mask, (char *)&notify_event);
    notify_event.response_type = XCB_LEAVE_NOTIFY;
    send_event(window, notify_mask, (char *)&notify_event);

    xcb_button_press_event_t button_event = {
        .response_type = XCB_BUTTON_PRESS,
        .detail = XCB_BUTTON_INDEX_1,
        .root = window->surface->window_id,
        .event = window->surface->window_id,
        .child = window->surface->window_id,
        0,
    };
    send_event(window, button_mask, (char *)&button_event);
    button_event.response_type = XCB_BUTTON_RELEASE;
    send_event(window, button_mask, (char *)&button_event);
}

struct comp_xwayland *
xwl_create(struct compositor *compositor) {
    struct comp_xwayland *xwl = calloc(1, sizeof(struct comp_xwayland));
    ww_assert(xwl);

    xwl->compositor = compositor;
    xwl->xwayland = wlr_xwayland_create(compositor->display, compositor->compositor, false);
    if (!xwl->xwayland) {
        wlr_log(WLR_ERROR, "xwl_create: failed to create wlr_xwayland");
        goto cleanup;
    }

    wl_list_init(&xwl->windows);

    xwl->on_new_surface.notify = handle_new_surface;
    wl_signal_add(&xwl->xwayland->events.new_surface, &xwl->on_new_surface);

    xwl->on_ready.notify = handle_ready;
    wl_signal_add(&xwl->xwayland->events.ready, &xwl->on_ready);

    wl_signal_init(&xwl->events.window_map);
    wl_signal_init(&xwl->events.window_unmap);
    wl_signal_init(&xwl->events.window_destroy);

    return xwl;

cleanup:
    free(xwl);
    return NULL;
}

void
xwl_destroy(struct comp_xwayland *xwl) {
    wl_list_remove(&xwl->on_new_surface.link);
    wl_list_remove(&xwl->on_ready.link);

    xcb_disconnect(xwl->xcb);
    xwm_destroy(xwl->xwayland->xwm);
    wlr_xwayland_destroy(xwl->xwayland);

    free(xwl);
}

void
xwl_send_keys(struct xwl_window *window, const struct synthetic_key *keys, size_t count) {
    ww_assert(window);
    ww_assert(keys);

    for (size_t i = 0; i < count; i++) {
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
        send_event(window, XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE, (char *)&event);
    }
}

void
xwl_update_cursor(struct comp_xwayland *xwl) {
    // The theme should already be loaded. If so, this simply returns it.
    if (!wlr_xcursor_manager_load(xwl->compositor->input->cursor_manager, 1)) {
        wlr_log(WLR_ERROR, "xwl_update_cursor: failed to load cursor theme");
        return;
    }

    struct wlr_xcursor *cursor =
        wlr_xcursor_manager_get_xcursor(xwl->compositor->input->cursor_manager, "default", 1);
    ww_assert(cursor);
    if (!cursor->image_count) {
        wlr_log(WLR_ERROR, "xwl_update_cursor: default cursor has no images");
        return;
    }
    struct wlr_xcursor_image *image = cursor->images[0];
    wlr_xwayland_set_cursor(xwl->xwayland, image->buffer, image->width * 4, image->width,
                            image->height, image->hotspot_x, image->hotspot_y);
}

void
xwl_window_activate(struct xwl_window *window) {
    wlr_xwayland_surface_activate(window->surface, true);
    wlr_xwayland_surface_restack(window->surface, NULL, XCB_STACK_MODE_ABOVE);
}

void
xwl_window_configure(struct xwl_window *window, struct wlr_box box) {
    wlr_xwayland_surface_configure(window->surface, box.x, box.y, box.width, box.height);
}

void
xwl_window_deactivate(struct xwl_window *window) {
    wlr_xwayland_surface_activate(window->surface, false);
}

void
xwl_window_set_floating(struct xwl_window *window) {
    window->floating = true;
}
