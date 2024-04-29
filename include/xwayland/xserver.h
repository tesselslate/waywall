#ifndef WAYWALL_XWAYLAND_XSERVER_H
#define WAYWALL_XWAYLAND_XSERVER_H

#include "xwayland/xwayland.h"
#include <sys/types.h>
#include <wayland-server-core.h>

struct xserver {
    struct wl_display *wl_display;
    struct wl_client *client;

    int display;
    char display_name[16];

    int fd_x11[2];
    int fd_xwm[2];
    int fd_wl[2];

    pid_t pid;
    int pidfd;

    struct wl_event_source *src_idle;
    struct wl_event_source *src_pidfd;
    struct wl_event_source *src_pipe;

    struct wl_listener on_client_destroy;

    struct {
        struct wl_signal ready;
    } events;
};

struct xserver *xserver_create(struct server_xwayland *xwl);
void xserver_destroy(struct xserver *srv);

#endif
