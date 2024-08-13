#ifndef WAYWALL_SERVER_XWAYLAND_H
#define WAYWALL_SERVER_XWAYLAND_H

#ifndef WAYWALL_XWAYLAND
#error xwayland.h should only be included with Xwayland support enabled
#endif

#include "server/ui.h"
#include "server/wl_seat.h"
#include <stdint.h>
#include <wayland-server-core.h>

struct server;

struct server_xwayland {
    struct server *server;
    struct server_xwayland_shell *shell;

    struct xserver *xserver;
    struct xwm *xwm;

    struct wl_listener on_display_destroy;
    struct wl_listener on_ready;

    struct {
        struct wl_signal ready;
    } events;
};

struct server_xwayland *server_xwayland_create(struct server *server,
                                               struct server_xwayland_shell *shell);

void xwl_notify_key(struct server_xwayland *xwl, uint32_t keycode, bool pressed);
void xwl_send_click(struct server_xwayland *xwl, struct server_view *view);
void xwl_send_keys(struct server_xwayland *xwl, struct server_view *view, size_t num_keys,
                   const struct syn_key keys[static num_keys]);
void xwl_set_clipboard(struct server_xwayland *xwl, const char *content);

#endif
