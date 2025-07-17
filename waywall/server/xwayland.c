#include "server/xwayland.h"
#include "server/server.h"
#include "server/xserver.h"
#include "server/xwm.h"
#include "string.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include <time.h>
#include <xcb/xproto.h>
#include <xcb/xtest.h>

/*
 * This code is partially my own making, but was largely only possible to write after combing
 * through other pre-existing implementations of Xwayland support. The licenses of codebases I
 * have referred to and used code from are included below.
 *
 * ==== weston
 *
 *  Copyright © 2008-2012 Kristian Høgsberg
 *  Copyright © 2010-2012 Intel Corporation
 *  Copyright © 2010-2011 Benjamin Franzke
 *  Copyright © 2011-2012 Collabora, Ltd.
 *  Copyright © 2010 Red Hat <mjg@redhat.com>
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a
 *  copy of this software and associated documentation files (the "Software"),
 *  to deal in the Software without restriction, including without limitation
 *  the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *  and/or sell copies of the Software, and to permit persons to whom the
 *  Software is furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice (including the next
 *  paragraph) shall be included in all copies or substantial portions of the
 *  Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 *  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *  DEALINGS IN THE SOFTWARE.
 *
 * ==== wlroots
 *
 *  Copyright (c) 2017, 2018 Drew DeVault
 *  Copyright (c) 2014 Jari Vetoniemi
 *  Copyright (c) 2023 The wlroots contributors
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy of
 *  this software and associated documentation files (the "Software"), to deal in
 *  the Software without restriction, including without limitation the rights to
 *  use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 *  of the Software, and to permit persons to whom the Software is furnished to do
 *  so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 */

static uint32_t
key_timestamp() {
    // HACK: Xwayland uses CLOCK_MONOTONIC, which uses a common epoch between applications on Linux.
    // Ideally, this will remain the case forever, because I don't want to replicate the awful time
    // approximation logic from resetti.

    // HACK: GLFW expects each keypress to have an ascending timestamp. We must make sure each
    // timestamp returned by this function is greater than the last.

    // TODO: If there ever exists any case where we issue two consecutive keypresses for the same
    // key, we must increment the timestamp by 20ms. This will need to track additional state at
    // that point.

    static uint32_t last_timestamp = 0;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t ms = now.tv_sec * 1000 + now.tv_nsec / 1000000;

    if (ms <= last_timestamp) {
        ms = ++last_timestamp;
    }
    last_timestamp = ms;

    return (uint32_t)ms;
}

static void
send_key_event(struct server_xwayland *xwl, xcb_window_t window, xcb_keycode_t keycode,
               bool press) {
    xcb_key_press_event_t event = {0};
    event.response_type = press ? XCB_KEY_PRESS : XCB_KEY_RELEASE;
    event.detail = keycode + 8; // Convert from libinput to XKB
    event.time = key_timestamp();
    event.root = window;
    event.event = window;
    event.child = window;
    event.same_screen = true;

    xcb_send_event(xwl->xwm->conn, true, window,
                   XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE, (char *)&event);
}

static void
on_ready(struct wl_listener *listener, void *data) {
    struct server_xwayland *xwl = wl_container_of(listener, xwl, on_ready);

    xwl->xwm = xwm_create(xwl, xwl->shell, xwl->xserver->fd_xwm[0]);
    if (!xwl->xwm) {
        return;
    }

    ww_log(LOG_INFO, "initialized X11 window manager");
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_xwayland *xwl = wl_container_of(listener, xwl, on_display_destroy);

    xserver_destroy(xwl->xserver);
    if (xwl->xwm) {
        xwm_destroy(xwl->xwm);
    }

    wl_list_remove(&xwl->on_display_destroy.link);

    free(xwl);
}

struct server_xwayland *
server_xwayland_create(struct server *server, struct server_xwayland_shell *shell) {
    struct server_xwayland *xwl = zalloc(1, sizeof(*xwl));

    xwl->server = server;
    xwl->shell = shell;

    xwl->xserver = xserver_create(xwl);
    if (!xwl->xserver) {
        free(xwl);
        return NULL;
    }

    xwl->on_ready.notify = on_ready;
    wl_signal_add(&xwl->xserver->events.ready, &xwl->on_ready);

    xwl->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &xwl->on_display_destroy);

    return xwl;
}

void
xwl_notify_key(struct server_xwayland *xwl, uint32_t keycode, bool pressed) {
    if (!xwl->xwm) {
        return;
    }

    xcb_test_fake_input(xwl->xwm->conn, pressed ? XCB_KEY_PRESS : XCB_KEY_RELEASE, keycode + 8,
                        XCB_CURRENT_TIME, xwl->xwm->screen->root, 0, 0, 0);
    xcb_flush(xwl->xwm->conn);
}

void
xwl_send_click(struct server_xwayland *xwl, struct server_view *view) {
    if (!xwl->xwm) {
        return;
    }

    // HACK: Sending an EnterNotify event causes GLFW to update the mouse pointer coordinates, so
    // we don't accidentally click any menu buttons.

    static const uint32_t window_mask = XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW;
    static const uint32_t button_mask = XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE;

    xcb_window_t window = xwm_window_from_view(view);

    // Send EnterNotify and LeaveNotify.
    xcb_enter_notify_event_t notify_event = {0};
    notify_event.response_type = XCB_ENTER_NOTIFY;
    notify_event.root = window;
    notify_event.event = window;
    notify_event.child = window;
    xcb_send_event(xwl->xwm->conn, true, window, window_mask, (char *)&notify_event);

    notify_event.response_type = XCB_LEAVE_NOTIFY;
    xcb_send_event(xwl->xwm->conn, true, window, window_mask, (char *)&notify_event);

    // Send a button press and release.
    xcb_button_press_event_t button_event = {0};
    button_event.response_type = XCB_BUTTON_PRESS;
    button_event.detail = XCB_BUTTON_INDEX_1;
    button_event.root = window;
    button_event.event = window;
    button_event.child = window;
    xcb_send_event(xwl->xwm->conn, true, window, button_mask, (char *)&button_event);

    button_event.response_type = XCB_BUTTON_RELEASE;
    xcb_send_event(xwl->xwm->conn, true, window, button_mask, (char *)&button_event);

    xcb_flush(xwl->xwm->conn);
}

void
xwl_send_keys(struct server_xwayland *xwl, struct server_view *view, size_t num_keys,
              const struct syn_key keys[static num_keys]) {
    if (!xwl->xwm) {
        return;
    }

    xcb_window_t window = xwm_window_from_view(view);

    for (size_t i = 0; i < num_keys; i++) {
        send_key_event(xwl, window, keys[i].keycode, keys[i].press);
    }

    xcb_flush(xwl->xwm->conn);
}

void
xwl_set_clipboard(struct server_xwayland *xwl, const char *content) {
    if (!xwl->xwm) {
        return;
    }

    xwm_set_clipboard(xwl->xwm, content);
}
