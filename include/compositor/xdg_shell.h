#ifndef WAYWALL_COMPOSITOR_XDG_SHELL_H
#define WAYWALL_COMPOSITOR_XDG_SHELL_H

#include "compositor/cutil.h"
#include <wayland-server-core.h>

struct server;
struct server_compositor;

struct server_xdg_wm_base {
    struct wl_global *global;

    struct server_compositor *compositor;

    struct wl_listener display_destroy;
};

struct server_xdg_surface {
    struct client_xdg_wm_base *wm_base;

    struct wl_list link;
    struct wl_resource *resource;

    struct server_surface *parent;
    struct server_xdg_toplevel *toplevel;

    struct ringbuf configure_serials;
    bool configured;

    struct wl_listener on_commit;
    struct wl_listener on_destroy;
};

struct server_xdg_toplevel {
    struct wl_resource *resource;

    struct server_xdg_surface *parent;
    struct server_toplevel_decoration *decoration;
    char *title;
    int32_t width, height;
    bool fullscreen;

    struct {
        struct wl_signal destroy;          // data: xdg_toplevel
        struct wl_signal unmap;            // data: xdg_toplevel
        struct wl_signal unset_fullscreen; // data: xdg_toplevel
        struct wl_signal set_fullscreen;   // data: xdg_toplevel
        struct wl_signal set_title;        // data: const char*
    } events;
};

struct server_xdg_wm_base *server_xdg_wm_base_create(struct server *server,
                                                     struct server_compositor *compositor);

void server_xdg_surface_send_configure(struct server_xdg_surface *xdg_surface);
struct server_xdg_surface *server_xdg_surface_from_resource(struct wl_resource *resource);
struct server_xdg_toplevel *server_xdg_toplevel_from_resource(struct wl_resource *resource);

#endif
