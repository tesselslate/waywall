/*
 *  The window_util module provides some functions for getting information about windows.
 */

#define WAYWALL_COMPOSITOR_IMPL

#include "compositor/pub_window_util.h"
#include "compositor/render.h"
#include "compositor/xwayland.h"
#include <wlr/xwayland.h>

void
window_close(struct window *window) {
    wlr_xwayland_surface_close(window->xwl_window->surface);
}

const char *
window_get_name(struct window *window) {
    return window->xwl_window->surface->title;
}

int
window_get_pid(struct window *window) {
    return window->xwl_window->surface->pid;
}
