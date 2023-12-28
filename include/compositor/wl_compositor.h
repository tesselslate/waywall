#ifndef WAYWALL_COMPOSITOR_WL_COMPOSITOR_H
#define WAYWALL_COMPOSITOR_WL_COMPOSITOR_H

#define COMPOSITOR_REMOTE_VERSION 4

#include "compositor/cutil.h"
#include <wayland-server-core.h>

struct server;

struct server_view {
    struct wl_list link; // server_compositor.output.views

    struct server_surface *surface;

    struct wl_subsurface *subsurface;
    struct wp_viewport *viewport;
    struct box bounds;

    // TODO: xwayland
    union {
        struct server_xdg_toplevel *xdg_shell;
    } data;
    enum {
        VIEW_XDG_SHELL,
    } type;

    struct wl_listener on_unmap;

    struct {
        struct wl_signal destroy; // data: server_view
    } events;
};

struct server_compositor {
    struct wl_compositor *remote;
    struct wl_global *global;

    struct {
        struct wl_display *remote_display;

        struct wl_subcompositor *subcompositor;
        struct wp_single_pixel_buffer_manager_v1 *single_pixel_buffer_manager;
        struct wp_viewporter *viewporter;
        struct xdg_wm_base *xdg_wm_base;

        struct wl_buffer *background;
        struct wl_surface *root_surface;
        struct wp_viewport *root_viewport;
        struct xdg_surface *xdg_surface;
        struct xdg_toplevel *xdg_toplevel;
        int width, height;
        bool mapped;

        struct wl_list views; // server_view.link
    } output;

    struct wl_listener display_destroy;
};

enum server_surface_role {
    ROLE_NONE,
    ROLE_CURSOR,       // no role object
    ROLE_XDG_TOPLEVEL, // xdg_toplevel resource

    // Not technically a role, but used when the surface has an associated xdg_surface but has not
    // yet been given an actual role. Has an xdg_surface resource.
    ROLE_XDG_SURFACE,
};

/*
 *  Used to hold pending state for a client's surface. We want to keep this separate
 *  from whatever state our compositor wants to commit (e.g. for subsurfaces and viewporter.)
 *  We ignore certain aspects we do not want the clients to be allowed to change (e.g. the
 *  input region.)
 */
struct surface_client_state {
    // wl_surface.attach
    struct server_buffer *buffer;

    // wl_surface.damage | wl_surface.damage_buffer
    struct wl_array damage, buffer_damage;

    // wl_surface.set_buffer_scale | wl_surface.set_buffer_transform
    int32_t scale;

    enum {
        SURFACE_STATE_BUFFER = (1 << 0),
        SURFACE_STATE_DAMAGE = (1 << 1),
        SURFACE_STATE_BUFFER_DAMAGE = (1 << 2),
        SURFACE_STATE_SCALE = (1 << 4),
    } changes;
};

struct server_surface {
    struct wl_resource *resource;
    struct wl_surface *remote;

    enum server_surface_role role;
    struct wl_resource *role_object;

    struct server_buffer *current_buffer;
    struct surface_client_state pending;

    struct {
        struct wl_signal commit;  // data: surface
        struct wl_signal destroy; // data: surface
    } events;

    struct wl_listener on_role_object_destroy;
};

struct server_region {
    struct wl_resource *resource;
    struct wl_region *remote;
};

struct server_compositor *server_compositor_create(struct server *server,
                                                   struct wl_compositor *remote);
void server_compositor_hide_window(struct server_compositor *compositor);
void server_compositor_show_window(struct server_compositor *compositor);

void server_surface_set_role_object(struct server_surface *surface, struct wl_resource *resource);

struct server_view *server_view_create_toplevel(struct server_compositor *compositor,
                                                struct server_xdg_toplevel *toplevel);
struct server_view *server_view_from_toplevel(struct server_compositor *compositor,
                                              struct server_xdg_toplevel *toplevel);
void server_view_destroy(struct server_view *view);
struct server_surface *server_view_get_surface(struct server_view *view);

void server_view_place_above(struct server_view *view, struct wl_surface *sibling);
void server_view_place_below(struct server_view *view, struct wl_surface *sibling);
void server_view_set_pos(struct server_view *view, int32_t x, int32_t y);
void server_view_set_size(struct server_view *view, int32_t width, int32_t height);
void server_view_set_source(struct server_view *view, double x, double y, double width,
                            double height);

struct server_compositor *server_compositor_from_resource(struct wl_resource *resource);
struct server_region *server_region_from_resource(struct wl_resource *resource);
struct server_surface *server_surface_from_resource(struct wl_resource *resource);

#endif
