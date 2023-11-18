#ifndef WAYWALL_COMPOSITOR_XWAYLAND_H
#define WAYWALL_COMPOSITOR_XWAYLAND_H

#include "compositor/compositor.h"
#include "compositor/input.h"
#include <wlr/util/box.h>

/*
 *  Contains Xwayland state for the compositor.
 *  Depends on the input subsystem.
 */
struct comp_xwayland {
    struct compositor *compositor;

    struct wlr_xwayland *xwayland;
    struct xcb_connection_t *xcb;
    struct wl_list windows;

    struct wl_listener on_new_surface;
    struct wl_listener on_ready;

    struct {
        struct wl_signal window_map;     // data: xwl_window
        struct wl_signal window_unmap;   // data: xwl_window
        struct wl_signal window_destroy; // data: xwl_window
    } events;
};

/*
 *  Contains information about a single Xwayland window.
 */
struct xwl_window {
    struct wl_list link;
    struct comp_xwayland *xwl;

    struct wlr_xwayland_surface *surface;
    bool mapped, floating;

    struct wl_listener on_associate;
    struct wl_listener on_dissociate;

    struct wl_listener on_map;
    struct wl_listener on_unmap;
    struct wl_listener on_destroy;
    struct wl_listener on_request_activate;
    struct wl_listener on_request_configure;
    struct wl_listener on_request_fullscreen;
    struct wl_listener on_request_minimize;

    struct {
        struct wl_signal map;       // data: xwl_window
        struct wl_signal unmap;     // data: xwl_window
        struct wl_signal configure; // data: wlr_xwayland_surface_configure_event
        struct wl_signal minimize;  // data: wlr_xwayland_minimize_event
        struct wl_signal destroy;   // data: xwl_window
    } events;
};

/*
 * Sends a fake mouse click to the given window.
 */
void xwl_click(struct xwl_window *window);

/*
 *  Attempts to set up Xwayland functionality for the compositor.
 */
struct comp_xwayland *xwl_create(struct compositor *compositor);

/*
 *  Frees all resources associated with the Xwayland connection.
 */
void xwl_destroy(struct comp_xwayland *xwl);

/*
 *  Sends a set of synthetic keypresses to the given window.
 */
void xwl_send_keys(struct xwl_window *window, const struct synthetic_key *keys, size_t count);

/*
 *  Updates the cursor image, which is used on the wall and in Minecraft.
 */
void xwl_update_cursor(struct comp_xwayland *xwl);

/*
 *  Communicates to the given window that it has been activated (received focus.)
 */
void xwl_window_activate(struct xwl_window *window);

/*
 *  Changes the size and position of the given window in the X environment.
 */
void xwl_window_configure(struct xwl_window *window, struct wlr_box box);

/*
 *  Deactivates the given window.
 */
void xwl_window_deactivate(struct xwl_window *window);

/*
 *  Marks the given window as a floating window, allowing it to configure itself.
 */
void xwl_window_set_floating(struct xwl_window *window);

#endif
