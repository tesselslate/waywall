#include "server/xwm.h"
#include "server/server.h"
#include "server/ui.h"
#include "server/wl_compositor.h"
#include "server/xserver.h"
#include "server/xwayland.h"
#include "server/xwayland_shell.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include <string.h>
#include <sys/types.h>
#include <wayland-server-core.h>
#include <xcb/composite.h>
#include <xcb/res.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/xtest.h>

/*
 * Additional reading:
 * https://www.x.org/releases/X11R7.6/doc/xorg-docs/specs/ICCCM/icccm.html
 * https://specifications.freedesktop.org/wm-spec/wm-spec-1.3.html
 *
 * NOTES:
 *
 * This window manager implementation is not actually compliant with either spec. GLFW requires next
 * to nothing from them, and it's the only client that we care about supporting.
 *
 *
 *
 * xwm.surfaces contains a `struct xsurface` for *every* X11 window, paired and unpaired. If a
 * window is paired with a Wayland surface, then `xsurface.parent` will be non-nullptr.
 * `xsurface.association` may contain data to indicate an association before it has actually been
 * made, so that other event handlers can look at that data to form the association.
 *
 * xwm.unpaired_shell contains a `struct unpaired_surface` for every xwayland_surface_v1 object
 * created using the xwayland-shell protocol. Once an association between the Wayland surface and an
 * X11 window is made, the `struct unpaired_surface` will be destroyed.
 *
 *
 *
 * One of the main jobs of this module is to associate X11 windows with Wayland surfaces. There are
 * two separate mechanisms that Xwayland might use to do this, depending on how old the user's
 * installed version is:
 *
 *   1. Xwayland will set the WL_SURFACE_ID atom on an X11 window to associate it with a
 *        wl_surface of the given object ID.
 *   2. Xwayland will set the WL_SURFACE_SERIAL atom on an X11 window and use the xwayland-shell
 *        protocol to associate a given wl_surface with that unique serial number.
 *
 * We need to be able to handle both methods of surface association and in both orders (i.e. Wayland
 * message before/after X11 message). The four cases are handled in the following places:
 *   1. WL_SURFACE_ID -> wl_surface creation    on_new_wl_surface
 *   2. wl_surface creation -> WL_SURFACE_ID    handle_msg_wl_surface_id
 *   3. WL_SURFACE_SERIAL -> set_serial         on_xwayland_surface_set_serial
 *   4. set_serial -> WL_SURFACE_SERIAL         handle_msg_wl_surface_serial
 *
 *
 *
 * A server_view should exist for a given X11 window only when all of the following conditions are
 * met:
 *   1. The X11 window is currently considered mapped in the X11 session (XMapWindow has been called
 *      with no associated unmap).
 *   2. There is a paired surface with a buffer attached.
 *
 * If a server_view exists for an X11 window and then one of the conditions is no longer met, it is
 * to be destroyed. All of this logic is implemented in `xsurface_update_view`, which is called
 * whenever these conditions may have changed.
 */

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

struct xsurface {
    struct wl_list link; // xwm.surfaces

    struct xwm *xwm;
    xcb_window_t window;

    struct {
        enum {
            ASSOC_SURFACE_ID,
            ASSOC_SURFACE_SERIAL,
            ASSOC_NONE,
        } type;
        uint64_t data;
    } association;

    struct server_surface *parent;
    char *title;
    pid_t pid;
    uint32_t width, height;

    struct server_view *view;
    bool mapped_x11;

    struct wl_listener on_surface_commit;
    struct wl_listener on_surface_destroy;
};

struct unpaired_surface {
    struct wl_list link; // xwm.unpaired_shell
    struct server_xwayland_surface *xwayland_surface;
    struct xwm *xwm;

    uint64_t serial;
    bool has_serial;

    struct wl_listener on_destroy;
    struct wl_listener on_set_serial;
};

static void on_surface_commit(struct wl_listener *listener, void *data);
static void on_surface_destroy(struct wl_listener *listener, void *data);
static void on_xwayland_surface_destroy(struct wl_listener *listener, void *data);
static void on_xwayland_surface_set_serial(struct wl_listener *listener, void *data);

static const char *atom_names[] = {
    [CLIPBOARD] = "CLIPBOARD",
    [NET_SUPPORTED] = "_NET_SUPPORTED",
    [NET_SUPPORTING_WM_CHECK] = "_NET_SUPPORTING_WM_CHECK",
    [NET_WM_NAME] = "_NET_WM_NAME",
    [NET_WM_STATE_FULLSCREEN] = "_NET_WM_STATE_FULLSCREEN",
    [TARGETS] = "TARGETS",
    [UTF8_STRING] = "UTF8_STRING",
    [WL_SURFACE_ID] = "WL_SURFACE_ID",
    [WL_SURFACE_SERIAL] = "WL_SURFACE_SERIAL",
    [WM_DELETE_WINDOW] = "WM_DELETE_WINDOW",
    [WM_PROTOCOLS] = "WM_PROTOCOLS",
    [WM_S0] = "WM_S0",
};

static void
xwayland_view_close(void *data) {
    struct xsurface *xsurface = data;

    // GLFW suppoorts WM_DELETE_WINDOW, so this is good enough.
    xcb_client_message_event_t event = {};
    event.response_type = XCB_CLIENT_MESSAGE;
    event.format = 32;
    event.data.data32[0] = xsurface->xwm->atoms[WM_DELETE_WINDOW];
    event.data.data32[1] = XCB_CURRENT_TIME;
    event.window = xsurface->window;
    event.type = xsurface->xwm->atoms[WM_PROTOCOLS];

    xcb_send_event(xsurface->xwm->conn, true, xsurface->window, XCB_EVENT_MASK_NO_EVENT,
                   (char *)&event);
    xcb_flush(xsurface->xwm->conn);
}

static pid_t
xwayland_view_get_pid(void *data) {
    struct xsurface *xsurface = data;

    return xsurface->pid;
}

static char *
xwayland_view_get_title(void *data) {
    struct xsurface *xsurface = data;

    if (xsurface->title) {
        return strdup(xsurface->title);
    } else {
        return nullptr;
    }
}

static void
xwayland_view_set_size(void *data, uint32_t width, uint32_t height) {
    struct xsurface *xsurface = data;
    struct xwm *xwm = xsurface->xwm;

    uint32_t values[] = {width, height};
    xcb_configure_window(xwm->conn, xsurface->window,
                         XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
    xcb_flush(xwm->conn);
}

static const struct server_view_impl xwayland_view_impl = {
    .name = "xwayland",

    .close = xwayland_view_close,
    .get_pid = xwayland_view_get_pid,
    .get_title = xwayland_view_get_title,
    .set_size = xwayland_view_set_size,
};

static pid_t
get_window_pid(struct xwm *xwm, xcb_window_t window) {
    xcb_res_client_id_spec_t spec = {};
    spec.client = window;
    spec.mask = XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID;

    xcb_res_query_client_ids_cookie_t cookie = xcb_res_query_client_ids(xwm->conn, 1, &spec);
    xcb_res_query_client_ids_reply_t *reply =
        xcb_res_query_client_ids_reply(xwm->conn, cookie, nullptr);
    if (!reply) {
        ww_log(LOG_WARN, "failed to query window PID with XRes");
        return -1;
    }

    pid_t pid = -1;
    xcb_res_client_id_value_iterator_t iter = xcb_res_query_client_ids_ids_iterator(reply);
    while (iter.rem > 0) {
        if (iter.data->spec.mask & XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID &&
            xcb_res_client_id_value_value_length(iter.data) > 0) {
            uint32_t *pid_ptr = xcb_res_client_id_value_value(iter.data);
            pid = *pid_ptr;
            break;
        }
        xcb_res_client_id_value_next(&iter);
    }

    free(reply);
    return pid;
}

static struct unpaired_surface *
upsurface_create(struct xwm *xwm, struct server_xwayland_surface *xwayland_surface) {
    struct unpaired_surface *upsurface = zalloc(1, sizeof(*upsurface));

    upsurface->xwayland_surface = xwayland_surface;
    upsurface->xwm = xwm;

    upsurface->on_destroy.notify = on_xwayland_surface_destroy;
    wl_signal_add(&xwayland_surface->events.destroy, &upsurface->on_destroy);

    upsurface->on_set_serial.notify = on_xwayland_surface_set_serial;
    wl_signal_add(&xwayland_surface->events.set_serial, &upsurface->on_set_serial);

    wl_list_insert(&xwm->unpaired_shell, &upsurface->link);

    return upsurface;
}

static void
upsurface_destroy(struct unpaired_surface *upsurface) {
    wl_list_remove(&upsurface->on_destroy.link);
    wl_list_remove(&upsurface->on_set_serial.link);

    wl_list_remove(&upsurface->link);
    free(upsurface);
}

static struct xsurface *
xsurface_create(struct xwm *xwm, xcb_window_t window) {
    // Subscribe to property changes on this window.
    static constexpr uint32_t MASK[] = {XCB_EVENT_MASK_PROPERTY_CHANGE};
    xcb_change_window_attributes(xwm->conn, window, XCB_CW_EVENT_MASK, MASK);

    // Create the xsurface object.
    struct xsurface *xsurface = zalloc(1, sizeof(*xsurface));

    xsurface->xwm = xwm;
    xsurface->window = window;

    xsurface->pid = get_window_pid(xwm, window);
    xsurface->association.type = ASSOC_NONE;

    wl_list_insert(&xwm->surfaces, &xsurface->link);

    ww_log(LOG_INFO, "xsurface created for window %" PRIu32, (uint32_t)window);
    return xsurface;
}

static void
xsurface_destroy(struct xsurface *xsurface) {
    if (xsurface->parent) {
        wl_list_remove(&xsurface->on_surface_commit.link);
        wl_list_remove(&xsurface->on_surface_destroy.link);
    }

    if (xsurface->title) {
        free(xsurface->title);
    }

    ww_assert(!xsurface->view);

    wl_list_remove(&xsurface->link);

    ww_log(LOG_INFO, "xsurface destroyed for window %" PRIu32, (uint32_t)xsurface->window);
    free(xsurface);
}

static inline struct xsurface *
xsurface_lookup(struct xwm *xwm, xcb_window_t window) {
    struct xsurface *surface;
    wl_list_for_each (surface, &xwm->surfaces, link) {
        if (surface->window == window) {
            return surface;
        }
    }

    return nullptr;
}

static void
xsurface_pair(struct xsurface *xsurface, struct server_surface *surface) {
    ww_assert(!xsurface->parent);

    xsurface->parent = surface;

    xsurface->on_surface_commit.notify = on_surface_commit;
    wl_signal_add(&surface->events.commit, &xsurface->on_surface_commit);

    xsurface->on_surface_destroy.notify = on_surface_destroy;
    wl_signal_add(&surface->events.destroy, &xsurface->on_surface_destroy);

    ww_log(LOG_INFO, "associated X11 window %" PRIu32 " with server_surface %p",
           (uint32_t)xsurface->window, surface);
}

static void
xsurface_unpair(struct xsurface *xsurface) {
    ww_assert(xsurface->parent);

    ww_log(LOG_INFO, "deassociated X11 window %" PRIu32 " with server_surface %p",
           (uint32_t)xsurface->window, xsurface->parent);

    xsurface->parent = nullptr;

    wl_list_remove(&xsurface->on_surface_commit.link);
    wl_list_remove(&xsurface->on_surface_destroy.link);
}

static void
xsurface_update_view(struct xsurface *xsurface, bool commit) {
    // If there is no associated Wayland surface, no server view should exist.
    if (!xsurface->parent) {
        ww_assert(!xsurface->view);
        return;
    }

    // If the associated Wayland surface does not have a buffer, it must not be given a server_view.
    bool has_buffer;
    if (commit) {
        // Determine whether or not the associated Wayland surface will have a buffer attached after
        // this commit completes.
        has_buffer = !!server_surface_next_buffer(xsurface->parent);
    } else {
        has_buffer = !!xsurface->parent->current.buffer;
    }

    // If the surface is not mapped in the X11 session, it should not be mapped by the compositor
    // either.
    bool mapped_x11 = xsurface->mapped_x11;

    // Determine whether or not the X11 window should have a server_view associated with it. Create
    // or destroy the server_view if necessary.
    bool should_map = has_buffer && mapped_x11;

    if (should_map && !xsurface->view) {
        xsurface->view = server_view_create(xsurface->xwm->server->ui, xsurface->parent,
                                            &xwayland_view_impl, xsurface);
        ww_assert(xsurface->view);
    } else if (!should_map && xsurface->view) {
        server_view_destroy(xsurface->view);
        xsurface->view = nullptr;
    }
}

static void
on_surface_commit(struct wl_listener *listener, void *data) {
    struct xsurface *xsurface = wl_container_of(listener, xsurface, on_surface_commit);

    xsurface_update_view(xsurface, true);
}

static void
on_surface_destroy(struct wl_listener *listener, void *data) {
    struct xsurface *xsurface = wl_container_of(listener, xsurface, on_surface_destroy);

    if (xsurface->view) {
        server_view_destroy(xsurface->view);
        xsurface->view = nullptr;
    }
    xsurface_unpair(xsurface);
}

static void
on_xwayland_surface_destroy(struct wl_listener *listener, void *data) {
    struct unpaired_surface *upsurface = wl_container_of(listener, upsurface, on_destroy);

    upsurface_destroy(upsurface);
}

static void
on_xwayland_surface_set_serial(struct wl_listener *listener, void *data) {
    struct unpaired_surface *upsurface = wl_container_of(listener, upsurface, on_set_serial);

    upsurface->serial = *(uint64_t *)data;
    upsurface->has_serial = true;

    // Attempt to find an existing X11 window with the given serial number in WL_SURFACE_SERIAL.
    struct xsurface *xsurface;
    wl_list_for_each (xsurface, &upsurface->xwm->surfaces, link) {
        // The X11 window must have WL_SURFACE_SERIAL set.
        if (xsurface->association.type != ASSOC_SURFACE_SERIAL) {
            continue;
        }

        // The content of the WL_SURFACE_SERIAL atom must match the serial from xwayland-shell.
        if (xsurface->association.data != upsurface->serial) {
            continue;
        }

        // There should not already be a pairing between this window and another surface.
        if (xsurface->parent) {
            ww_log(LOG_WARN,
                   "extraneous association between X11 window %" PRIu32
                   " and WL_SURFACE_SERIAL %" PRIu64,
                   (uint32_t)xsurface->window, upsurface->serial);
            return;
        }

        xsurface_pair(xsurface, upsurface->xwayland_surface->parent);
        return;
    }
}

static void
on_input_focus(struct wl_listener *listener, void *data) {
    struct xwm *xwm = wl_container_of(listener, xwm, on_input_focus);
    struct server_view *view = data;

    xcb_window_t target = XCB_WINDOW_NONE;
    if (view && strcmp(view->impl->name, "xwayland") == 0) {
        target = xwm_window_from_view(view);
    }

    xcb_set_input_focus(xwm->conn, XCB_INPUT_FOCUS_NONE, target, XCB_CURRENT_TIME);
}

static void
on_new_wl_surface(struct wl_listener *listener, void *data) {
    struct xwm *xwm = wl_container_of(listener, xwm, on_new_wl_surface);
    struct server_surface *surface = data;

    // If Xwayland is not the owner of the new surface, we don't care about it.
    struct wl_client *xwl_client = xwm->xserver->client;
    if (xwl_client != wl_resource_get_client(surface->resource)) {
        return;
    }

    uint32_t id = wl_resource_get_id(surface->resource);

    // Attempt to find an existing X11 window with the surface object ID in WL_SURFACE_ID.
    struct xsurface *xsurface;
    wl_list_for_each (xsurface, &xwm->surfaces, link) {
        // The X11 window must have WL_SURFACE_ID set.
        if (xsurface->association.type != ASSOC_SURFACE_ID) {
            continue;
        }

        // The content of the WL_SURFACE_ID atom must match the surface object ID.
        if (xsurface->association.data != id) {
            continue;
        }

        // There should not already be a pairing between this window and another surface.
        if (xsurface->parent) {
            ww_log(LOG_WARN,
                   "extraneous association between X11 window %" PRIu32
                   " and WL_SURFACE_ID %" PRIu32,
                   (uint32_t)xsurface->window, id);
            return;
        }

        xsurface_pair(xsurface, surface);
        return;
    }
}

static void
on_new_xwayland_surface(struct wl_listener *listener, void *data) {
    struct xwm *xwm = wl_container_of(listener, xwm, on_new_xwayland_surface);
    struct server_xwayland_surface *xwayland_surface = data;

    upsurface_create(xwm, xwayland_surface);
}

static void
handle_msg_wl_surface_id(struct xwm *xwm, xcb_client_message_event_t *event) {
    if (xwm->shell->bound) {
        ww_log(LOG_WARN, "Xwayland is using WL_SURFACE_ID despite binding xwayland-shell");
        return;
    }

    uint32_t id = event->data.data32[0];

    // There should not already be a pairing between this window and another surface.
    struct xsurface *xsurface = xsurface_lookup(xwm, event->window);
    if (xsurface->parent) {
        ww_log(LOG_WARN,
               "extraneous association between X11 window %" PRIu32 " and WL_SURFACE_ID %" PRIu32,
               (uint32_t)event->window, id);
        return;
    }

    xsurface->association.type = ASSOC_SURFACE_ID;
    xsurface->association.data = id;

    // Confirm that there is a wl_surface object with the given ID.
    struct wl_client *xwl_client = xwm->xserver->client;
    struct wl_resource *resource = wl_client_get_object(xwl_client, id);
    if (!resource) {
        return;
    }

    struct server_surface *surface = server_surface_try_from_resource(resource);
    if (!surface) {
        return;
    }

    xsurface_pair(xsurface, surface);
    return;
}

static void
handle_msg_wl_surface_serial(struct xwm *xwm, xcb_client_message_event_t *event) {
    uint32_t serial_lo = event->data.data32[0];
    uint32_t serial_hi = event->data.data32[1];
    uint64_t serial = (uint64_t)serial_lo | ((uint64_t)serial_hi) << 32;

    // There should not already be a pairing between this window and another surface.
    struct xsurface *xsurface = xsurface_lookup(xwm, event->window);
    if (xsurface->parent) {
        ww_log(LOG_WARN,
               "extraneous association between X11 window %" PRIu32
               " and WL_SURFACE_SERIAL %" PRIu64,
               (uint32_t)event->window, serial);
        return;
    }

    xsurface->association.type = ASSOC_SURFACE_SERIAL;
    xsurface->association.data = serial;

    // Check to see if there is an unpaired_surface waiting to pair with this X11 window.
    struct unpaired_surface *upsurface;
    wl_list_for_each (upsurface, &xwm->unpaired_shell, link) {
        if (upsurface->has_serial && upsurface->serial == serial) {
            xsurface_pair(xsurface, upsurface->xwayland_surface->parent);
            upsurface_destroy(upsurface);
            return;
        }
    }
}

static void
handle_xcb_client_message(struct xwm *xwm, xcb_client_message_event_t *event) {
    if (event->type == xwm->atoms[WL_SURFACE_ID]) {
        handle_msg_wl_surface_id(xwm, event);
    } else if (event->type == xwm->atoms[WL_SURFACE_SERIAL]) {
        handle_msg_wl_surface_serial(xwm, event);
    } else {
        // TODO: log unknown messages?
    }
}

static void
handle_xcb_configure_request(struct xwm *xwm, xcb_configure_request_event_t *event) {
    static constexpr uint32_t CONFIGURE_MASK = XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;

    xcb_configure_window(xwm->conn, event->window, CONFIGURE_MASK,
                         (uint32_t[2]){event->width, event->height});
}

static void
handle_xcb_create_notify(struct xwm *xwm, xcb_create_notify_event_t *event) {
    if (event->window == xwm->ewmh_window) {
        return;
    }

    if (event->override_redirect) {
        ww_log(LOG_WARN,
               "X11 client attempted to create window (%" PRIu32 ") with override redirect",
               (uint32_t)event->window);
        xcb_kill_client(xwm->conn, event->window);
        return;
    }

    xsurface_create(xwm, event->window);
}

static void
handle_xcb_destroy_notify(struct xwm *xwm, xcb_destroy_notify_event_t *event) {
    struct xsurface *xsurface = xsurface_lookup(xwm, event->window);

    if (!xsurface) {
        return;
    }

    xsurface_destroy(xsurface);
}

static void
handle_xcb_map_request(struct xwm *xwm, xcb_map_request_event_t *event) {
    struct xsurface *xsurface = xsurface_lookup(xwm, event->window);

    if (!xsurface) {
        return;
    }

    xcb_map_window(xwm->conn, event->window);
    xsurface->mapped_x11 = true;
    xsurface_update_view(xsurface, false);
}

static void
handle_xcb_property_notify(struct xwm *xwm, xcb_property_notify_event_t *event) {
    struct xsurface *xsurface = xsurface_lookup(xwm, event->window);

    if (!xsurface) {
        return;
    }

    // Currently, the only atom we care about is _NET_WM_NAME for the window title.
    if (event->atom != xwm->atoms[NET_WM_NAME]) {
        return;
    }

    // The response size limit of 4KB is arbitrary.
    xcb_get_property_cookie_t cookie =
        xcb_get_property(xwm->conn, 0, xsurface->window, event->atom, XCB_ATOM_ANY, 0, 4096);
    xcb_get_property_reply_t *reply = xcb_get_property_reply(xwm->conn, cookie, nullptr);
    if (!reply) {
        ww_log(LOG_ERROR, "failed to get property (window: %" PRIu32 ", atom: %" PRIu32 ")",
               (uint32_t)event->window, (uint32_t)event->atom);
        return;
    }

    // Read the window title from the reply.
    if (reply->type != XCB_ATOM_STRING && reply->type != xwm->atoms[UTF8_STRING]) {
        goto done;
    }

    size_t len = xcb_get_property_value_length(reply);
    char *title = xcb_get_property_value(reply);

    if (xsurface->title) {
        free(xsurface->title);
        xsurface->title = nullptr;
    }

    if (len > 0) {
        xsurface->title = strndup(title, len);
    }

done:
    free(reply);
    return;
}

static void
handle_xcb_selection_request(struct xwm *xwm, xcb_selection_request_event_t *event) {
    if (!xwm->paste_content) {
        ww_log(LOG_WARN, "X11 client requested clipboard content while none was set");
        return;
    }

    // We only care about the clipboard and not any other data transfer (e.g. primary selection.)
    if (event->selection != xwm->atoms[CLIPBOARD]) {
        return;
    }

    xcb_generic_error_t *err;
    xcb_get_selection_owner_cookie_t owner_cookie =
        xcb_get_selection_owner(xwm->conn, xwm->atoms[CLIPBOARD]);
    xcb_get_selection_owner_reply_t *owner_reply =
        xcb_get_selection_owner_reply(xwm->conn, owner_cookie, &err);
    if (err) {
        // TODO: Handle error
        return;
    }

    // If we do not own the selection anymore, do not send a response.
    if (owner_reply->owner != xwm->ewmh_window) {
        free(owner_reply);
        return;
    }
    free(owner_reply);

    if (event->target == xwm->atoms[TARGETS] && event->property != XCB_ATOM_NONE) {
        xcb_change_property(xwm->conn, XCB_PROP_MODE_REPLACE, event->requestor, event->property,
                            XCB_ATOM_ATOM, 32, 1, (uint32_t[1]){xwm->atoms[UTF8_STRING]});
    } else if (event->target == xwm->atoms[UTF8_STRING] && event->property != XCB_ATOM_NONE) {
        xcb_change_property(xwm->conn, XCB_PROP_MODE_REPLACE, event->requestor, event->property,
                            XCB_ATOM_ATOM, 8, strlen(xwm->paste_content), xwm->paste_content);
    }

    xcb_selection_notify_event_t evt = {};
    evt.response_type = XCB_SELECTION_NOTIFY;
    evt.sequence = event->sequence;
    evt.requestor = event->requestor;
    evt.selection = event->selection;
    evt.target = event->target;
    evt.property = event->property;
    evt.time = event->time;

    xcb_send_event(xwm->conn, 0, event->requestor, XCB_EVENT_MASK_NO_EVENT, (const char *)&evt);
    xcb_flush(xwm->conn);
}

static void
handle_xcb_unmap_notify(struct xwm *xwm, xcb_unmap_notify_event_t *event) {
    struct xsurface *xsurface = xsurface_lookup(xwm, event->window);

    if (!xsurface) {
        return;
    }

    xsurface->mapped_x11 = false;
    xsurface_update_view(xsurface, false);
}

static void
handle_xcb_error(struct xwm *xwm, xcb_value_error_t *event) {
    ww_log(LOG_ERROR,
           "xcb error: opcode %" PRIu8 ":%" PRIu16 ", error code %" PRIu8 ", sequence %" PRIu16
           ", value %" PRIu32,
           event->major_opcode, event->minor_opcode, event->error_code, event->sequence,
           event->bad_value);
}

static void
handle_xcb_unknown(struct xwm *xwm, xcb_generic_event_t *event) {
    ww_log(LOG_INFO, "unhandled X11 event (type: %u)", event->response_type);
}

static int
handle_x11_conn(int32_t fd, uint32_t mask, void *data) {
    struct xwm *xwm = data;

    int count = 0;
    xcb_generic_event_t *event;

    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        wl_event_source_remove(xwm->src_x11);
        xwm->src_x11 = nullptr;
        return 0;
    }

    while ((event = xcb_poll_for_event(xwm->conn))) {
        count++;

        switch (event->response_type & ~0x80) {
        case XCB_CLIENT_MESSAGE:
            handle_xcb_client_message(xwm, (xcb_client_message_event_t *)event);
            break;
        case XCB_CONFIGURE_NOTIFY:
            // Unused.
            break;
        case XCB_CONFIGURE_REQUEST:
            handle_xcb_configure_request(xwm, (xcb_configure_request_event_t *)event);
            break;
        case XCB_CREATE_NOTIFY:
            handle_xcb_create_notify(xwm, (xcb_create_notify_event_t *)event);
            break;
        case XCB_DESTROY_NOTIFY:
            handle_xcb_destroy_notify(xwm, (xcb_destroy_notify_event_t *)event);
            break;
        case XCB_MAP_NOTIFY:
            // Unused.
            break;
        case XCB_MAPPING_NOTIFY:
            // Unused.
            break;
        case XCB_MAP_REQUEST:
            handle_xcb_map_request(xwm, (xcb_map_request_event_t *)event);
            break;
        case XCB_PROPERTY_NOTIFY:
            handle_xcb_property_notify(xwm, (xcb_property_notify_event_t *)event);
            break;
        case XCB_SELECTION_REQUEST:
            handle_xcb_selection_request(xwm, (xcb_selection_request_event_t *)event);
            break;
        case XCB_UNMAP_NOTIFY:
            handle_xcb_unmap_notify(xwm, (xcb_unmap_notify_event_t *)event);
            break;
        case 0:
            handle_xcb_error(xwm, (xcb_value_error_t *)event);
            break;
        default:
            handle_xcb_unknown(xwm, event);
            break;
        }

        free(event);
    }

    if (count > 0) {
        xcb_flush(xwm->conn);
    }

    return (count > 0) ? 1 : 0;
}

static void
init_xres(struct xwm *xwm) {
    const xcb_query_extension_reply_t *query_xres = xcb_get_extension_data(xwm->conn, &xcb_res_id);
    if (!query_xres || !query_xres->present) {
        ww_log(LOG_WARN, "XRes extension not present");
        return;
    }

    xcb_res_query_version_cookie_t cookie =
        xcb_res_query_version(xwm->conn, XCB_RES_MAJOR_VERSION, XCB_RES_MINOR_VERSION);
    xcb_res_query_version_reply_t *reply = xcb_res_query_version_reply(xwm->conn, cookie, nullptr);
    if (!reply) {
        ww_log(LOG_WARN, "failed to query XRes extension version");
        return;
    }

    ww_log(LOG_INFO, "XRes extension version: %" PRIu32 ".%" PRIu32, reply->server_major,
           reply->server_minor);

    if (reply->server_major > 1 || (reply->server_major == 1 && reply->server_minor >= 2)) {
        xwm->extensions.xres = true;
    }

    free(reply);
}

static void
init_xtest(struct xwm *xwm) {
    const xcb_query_extension_reply_t *query_xtest =
        xcb_get_extension_data(xwm->conn, &xcb_test_id);
    if (!query_xtest || !query_xtest->present) {
        ww_log(LOG_ERROR, "XTEST extension not present");
        return;
    }

    xcb_test_get_version_cookie_t cookie =
        xcb_test_get_version(xwm->conn, XCB_TEST_MAJOR_VERSION, XCB_TEST_MINOR_VERSION);
    xcb_test_get_version_reply_t *reply = xcb_test_get_version_reply(xwm->conn, cookie, nullptr);
    if (!reply) {
        ww_log(LOG_WARN, "failed to query XTEST extension version");
        return;
    }

    ww_log(LOG_INFO, "XTEST extension version: %" PRIu32 ".%" PRIu32, reply->major_version,
           reply->minor_version);

    if (reply->major_version > 2 || (reply->major_version == 2 && reply->minor_version >= 2)) {
        xwm->extensions.xtest = true;
    }

    free(reply);
}

static int
init_atoms(struct xwm *xwm) {
    // Get all of the required atoms.
    xcb_intern_atom_cookie_t cookies[ATOM_COUNT] = {};

    // Make all of the requests up front and then check them after.
    for (size_t i = 0; i < ATOM_COUNT; i++) {
        cookies[i] = xcb_intern_atom(xwm->conn, 0, strlen(atom_names[i]), atom_names[i]);
    }

    for (size_t i = 0; i < ATOM_COUNT; i++) {
        xcb_generic_error_t *error;
        xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(xwm->conn, cookies[i], &error);

        if (error) {
            ww_log(LOG_ERROR, "failed to resolve X11 atom '%s' (error code: %d)", atom_names[i],
                   error->error_code);
            free(error);
            return 1;
        }

        ww_assert(reply);
        xwm->atoms[i] = reply->atom;
        free(reply);
    }

    return 0;
}

static void
init_wm(struct xwm *xwm) {
    static constexpr uint32_t MASK[] = {XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                                        XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                                        XCB_EVENT_MASK_PROPERTY_CHANGE};
    xcb_change_window_attributes(xwm->conn, xwm->screen->root, XCB_CW_EVENT_MASK, MASK);
}

static void
init_ewmh(struct xwm *xwm) {
    static constexpr char wm_name[] = "wm";

    xwm->ewmh_window = xcb_generate_id(xwm->conn);
    xcb_create_window(xwm->conn, XCB_COPY_FROM_PARENT, xwm->ewmh_window, xwm->screen->root, 0, 0, 1,
                      1, 0, XCB_WINDOW_CLASS_INPUT_ONLY, xwm->screen->root_visual, 0, nullptr);

    xcb_change_property(xwm->conn, XCB_PROP_MODE_REPLACE, xwm->ewmh_window, xwm->atoms[NET_WM_NAME],
                        xwm->atoms[UTF8_STRING], 8, STATIC_STRLEN(wm_name), wm_name);

    xcb_change_property(xwm->conn, XCB_PROP_MODE_REPLACE, xwm->ewmh_window,
                        xwm->atoms[NET_SUPPORTING_WM_CHECK], XCB_ATOM_WINDOW, 32, 1,
                        &xwm->screen->root);
    xcb_change_property(xwm->conn, XCB_PROP_MODE_REPLACE, xwm->screen->root,
                        xwm->atoms[NET_SUPPORTING_WM_CHECK], XCB_ATOM_WINDOW, 32, 1,
                        &xwm->ewmh_window);

    xcb_atom_t supported[] = {
        xwm->atoms[NET_WM_STATE_FULLSCREEN],
    };

    xcb_change_property(xwm->conn, XCB_PROP_MODE_REPLACE, xwm->screen->root,
                        xwm->atoms[NET_SUPPORTED], XCB_ATOM_ATOM, 32, STATIC_ARRLEN(supported),
                        supported);

    xcb_set_selection_owner(xwm->conn, xwm->ewmh_window, xwm->atoms[WM_S0], XCB_CURRENT_TIME);
}

struct xwm *
xwm_create(struct server_xwayland *xwl, struct server_xwayland_shell *shell, int xwm_fd) {
    struct xwm *xwm = zalloc(1, sizeof(*xwm));

    xwm->server = xwl->server;
    xwm->xserver = xwl->xserver;
    xwm->shell = shell;

    wl_list_init(&xwm->surfaces);
    wl_list_init(&xwm->unpaired_shell);

    // Setup the XCB connection.
    xwm->conn = xcb_connect_to_fd(xwm_fd, nullptr);
    int err = xcb_connection_has_error(xwm->conn);
    if (err) {
        ww_log(LOG_ERROR, "xcb connection failed: %d", err);
        goto fail_xcb_connect;
    }

    xcb_screen_iterator_t screen_iter = xcb_setup_roots_iterator(xcb_get_setup(xwm->conn));
    xwm->screen = screen_iter.data;
    ww_assert(xwm->screen);

    // Initialize the event handler for managing the X11 connection.
    xwm->src_x11 = wl_event_loop_add_fd(wl_display_get_event_loop(xwl->server->display), xwm_fd,
                                        WL_EVENT_READABLE, handle_x11_conn, xwm);
    check_alloc(xwm->src_x11);
    wl_event_source_check(xwm->src_x11);

    if (init_atoms(xwm) != 0) {
        goto fail_resources;
    }

    xcb_prefetch_extension_data(xwm->conn, &xcb_composite_id);
    xcb_prefetch_extension_data(xwm->conn, &xcb_res_id);

    init_xres(xwm);
    if (!xwm->extensions.xres) {
        // TODO: If XRes support is uncommon, we can support _NET_WM_PID
        ww_log(LOG_ERROR, "no XRes support");
        goto fail_xres;
    }

    init_xtest(xwm);
    if (!xwm->extensions.xtest) {
        ww_log(LOG_ERROR, "no XTEST support");
        goto fail_xtest;
    }

    init_wm(xwm);
    init_ewmh(xwm);

    // We don't actually do anything with the Xcomposite extension. We just need to do this or else
    // Xwayland will refuse to associate X11 windows with Wayland surfaces.
    xcb_composite_redirect_subwindows(xwm->conn, xwm->screen->root, XCB_COMPOSITE_REDIRECT_MANUAL);

    xcb_flush(xwm->conn);

    xwm->on_input_focus.notify = on_input_focus;
    wl_signal_add(&xwl->server->events.input_focus, &xwm->on_input_focus);

    xwm->on_new_wl_surface.notify = on_new_wl_surface;
    wl_signal_add(&xwl->server->compositor->events.new_surface, &xwm->on_new_wl_surface);

    xwm->on_new_xwayland_surface.notify = on_new_xwayland_surface;
    wl_signal_add(&xwm->shell->events.new_surface, &xwm->on_new_xwayland_surface);

    wl_signal_init(&xwm->events.clipboard);

    return xwm;

fail_xtest:
fail_xres:
fail_resources:
    wl_event_source_remove(xwm->src_x11);
    xcb_disconnect(xwm->conn);

fail_xcb_connect:
    free(xwm);
    return nullptr;
}

void
xwm_destroy(struct xwm *xwm) {
    xcb_disconnect(xwm->conn);

    // The X11 pipe event source will have been removed already if the connection died.
    if (xwm->src_x11) {
        wl_event_source_remove(xwm->src_x11);
    }

    wl_list_remove(&xwm->on_input_focus.link);
    wl_list_remove(&xwm->on_new_wl_surface.link);
    wl_list_remove(&xwm->on_new_xwayland_surface.link);

    struct xsurface *xsurface, *tmp_xsurface;
    wl_list_for_each_safe (xsurface, tmp_xsurface, &xwm->surfaces, link) {
        xsurface_destroy(xsurface);
    }

    struct unpaired_surface *upsurface, *tmp_upsurface;
    wl_list_for_each_safe (upsurface, tmp_upsurface, &xwm->unpaired_shell, link) {
        upsurface_destroy(upsurface);
    }

    if (xwm->paste_content) {
        free(xwm->paste_content);
    }

    free(xwm);
}

void
xwm_set_clipboard(struct xwm *xwm, const char *content) {
    if (xwm->paste_content) {
        free(xwm->paste_content);
    }
    xwm->paste_content = strdup(content);
    check_alloc(xwm->paste_content);

    xcb_set_selection_owner(xwm->conn, xwm->ewmh_window, xwm->atoms[CLIPBOARD], XCB_CURRENT_TIME);
}

xcb_window_t
xwm_window_from_view(struct server_view *view) {
    ww_assert(strcmp(view->impl->name, "xwayland") == 0);

    struct xsurface *xsurface = view->impl_data;
    return xsurface->window;
}
