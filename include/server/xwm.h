#ifndef WAYWALL_SERVER_XWM_H
#define WAYWALL_SERVER_XWM_H

#include "server/xwayland.h"
#include <wayland-server-core.h>
#include <xcb/xcb.h>

enum xwm_atom {
    CLIPBOARD,
    NET_SUPPORTED,
    NET_SUPPORTING_WM_CHECK,
    NET_WM_NAME,
    NET_WM_STATE_FULLSCREEN,
    TARGETS,
    UTF8_STRING,
    WL_SURFACE_ID,
    WL_SURFACE_SERIAL,
    WM_DELETE_WINDOW,
    WM_S0,

    ATOM_COUNT,
};

struct xwm {
    struct server *server;
    struct xserver *xserver;
    struct server_xwayland_shell *shell;

    xcb_connection_t *conn;
    xcb_screen_t *screen;

    xcb_window_t ewmh_window;
    xcb_atom_t atoms[ATOM_COUNT];

    char *paste_content;

    struct {
        bool xres;
        bool xtest;
    } extensions;

    struct wl_list surfaces;       // xsurface.link
    struct wl_list unpaired_shell; // unpaired_surface.link

    struct wl_event_source *src_x11;

    struct wl_listener on_input_focus;
    struct wl_listener on_new_wl_surface;
    struct wl_listener on_new_xwayland_surface;

    struct {
        struct wl_signal clipboard; // data: const char *
    } events;
};

struct xwm *xwm_create(struct server_xwayland *xwl, struct server_xwayland_shell *shell,
                       int xwm_fd);
void xwm_destroy(struct xwm *xwm);

void xwm_set_clipboard(struct xwm *xwm, const char *content);
xcb_window_t xwm_window_from_view(struct server_view *view);

#endif
