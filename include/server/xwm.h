#ifndef WAYWALL_SERVER_XWM_H
#define WAYWALL_SERVER_XWM_H

#ifndef WAYWALL_XWAYLAND
#error xwm.h should only be included with Xwayland support enabled
#endif

#include "server/xwayland.h"
#include <wayland-server-core.h>
#include <xcb/xcb.h>
#include <xcb/xcb_errors.h>

enum xwm_atom {
    NET_SUPPORTED,
    NET_SUPPORTING_WM_CHECK,
    NET_WM_NAME,
    UTF8_STRING,
    WL_SURFACE_ID,
    WL_SURFACE_SERIAL,

    ATOM_COUNT,
};

struct xwm {
    struct server *server;
    struct xserver *xserver;
    struct server_xwayland_shell *shell;

    xcb_connection_t *conn;
    xcb_errors_context_t *errctx;
    xcb_screen_t *screen;

    xcb_window_t ewmh_window;
    xcb_atom_t atoms[ATOM_COUNT];

    struct {
        bool xres;
    } extensions;

    struct wl_list surfaces;       // xsurface.link
    struct wl_list unpaired_shell; // unpaired_surface.link

    struct wl_event_source *src_x11;

    struct wl_listener on_new_wl_surface;
    struct wl_listener on_new_xwayland_surface;
};

struct xwm *xwm_create(struct server_xwayland *xwl, struct server_xwayland_shell *shell,
                       int xwm_fd);
void xwm_destroy(struct xwm *xwm);

#endif
