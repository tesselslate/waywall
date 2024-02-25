#include "server/xdg_shell.h"
#include "server/server.h"
#include "server/wl_compositor.h"
#include "util.h"
#include "xdg-shell-server-protocol.h"
#include <stdlib.h>

/*
 * TODO: It may be worth implementing ping/pong support at a later date to detect if instances
 * crash/hang.
 */

#define SRV_XDG_WM_BASE_VERSION 5

static void
send_toplevel_configure(struct server_xdg_toplevel *xdg_toplevel) {
    struct wl_array states;
    wl_array_init(&states);
    xdg_toplevel_send_configure(xdg_toplevel->resource, xdg_toplevel->width, xdg_toplevel->height,
                                &states);
    wl_array_release(&states);
}

static void
on_surface_commit(struct wl_listener *listener, void *data) {
    struct server_xdg_surface *xdg_surface = wl_container_of(listener, xdg_surface, on_commit);
    struct server_surface *surface = xdg_surface->parent;

    if (!xdg_surface->initial_ack) {
        if (surface->pending.buffer) {
            wl_resource_post_error(
                xdg_surface->resource, XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
                "cannot call wl_surface.commit with buffer before configuring xdg_surface");
            return;
        }
    }
    if (!xdg_surface->initial_commit) {
        xdg_surface->initial_commit = true;

        if (xdg_surface->child) {
            send_toplevel_configure(xdg_surface->child);

            struct wl_array capabilities;
            wl_array_init(&capabilities);
            xdg_toplevel_send_wm_capabilities(xdg_surface->child->resource, &capabilities);
            wl_array_release(&capabilities);
        }
        server_xdg_surface_send_configure(xdg_surface);
    }
}

static void
on_surface_destroy(struct wl_listener *listener, void *data) {
    struct server_xdg_surface *xdg_surface = wl_container_of(listener, xdg_surface, on_destroy);

    wl_resource_post_error(xdg_surface->resource, XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,
                           "wl_surface destroyed before associated xdg_surface");
}

static void
xdg_toplevel_resource_destroy(struct wl_resource *resource) {
    struct server_xdg_toplevel *xdg_toplevel = wl_resource_get_user_data(resource);

    wl_signal_emit_mutable(&xdg_toplevel->events.destroy, NULL);

    xdg_toplevel->parent->child = NULL;
    xdg_toplevel->parent->parent->role_object = NULL;

    if (xdg_toplevel->title) {
        free(xdg_toplevel->title);
    }
    free(xdg_toplevel);
}

static void
xdg_toplevel_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
xdg_toplevel_move(struct wl_client *client, struct wl_resource *resource,
                  struct wl_resource *seat_resource, uint32_t serial) {
    // Unused.
}

static void
xdg_toplevel_resize(struct wl_client *client, struct wl_resource *resource,
                    struct wl_resource *seat_resource, uint32_t serial, uint32_t edges) {
    // Unused.
}

static void
xdg_toplevel_set_app_id(struct wl_client *client, struct wl_resource *resource,
                        const char *app_id) {
    // Unused.
}

static void
xdg_toplevel_set_fullscreen(struct wl_client *client, struct wl_resource *resource,
                            struct wl_resource *output_resource) {
    struct server_xdg_toplevel *xdg_toplevel = wl_resource_get_user_data(resource);

    // This is a no-op, but we need to send a configure anyway.
    send_toplevel_configure(xdg_toplevel);
    server_xdg_surface_send_configure(xdg_toplevel->parent);
}

static void
xdg_toplevel_set_maximized(struct wl_client *client, struct wl_resource *resource) {
    struct server_xdg_toplevel *xdg_toplevel = wl_resource_get_user_data(resource);

    // This is a no-op, but we need to send a configure anyway.
    send_toplevel_configure(xdg_toplevel);
    server_xdg_surface_send_configure(xdg_toplevel->parent);
}

static void
xdg_toplevel_set_minimized(struct wl_client *client, struct wl_resource *resource) {
    // Unused.
}

static void
xdg_toplevel_set_max_size(struct wl_client *client, struct wl_resource *resource, int32_t width,
                          int32_t height) {
    // Unused.
}

static void
xdg_toplevel_set_min_size(struct wl_client *client, struct wl_resource *resource, int32_t width,
                          int32_t height) {
    // Unused.
}

static void
xdg_toplevel_set_parent(struct wl_client *client, struct wl_resource *resource,
                        struct wl_resource *toplevel_resource) {
    // Unused.
    wl_client_post_implementation_error(client, "xdg_toplevel.set_parent is not supported");
}

static void
xdg_toplevel_set_title(struct wl_client *client, struct wl_resource *resource, const char *title) {
    struct server_xdg_toplevel *xdg_toplevel = wl_resource_get_user_data(resource);

    if (xdg_toplevel->title) {
        free(xdg_toplevel->title);
    }
    xdg_toplevel->title = strdup(title);
}

static void
xdg_toplevel_show_window_menu(struct wl_client *client, struct wl_resource *resource,
                              struct wl_resource *seat_resource, uint32_t serial, int32_t x,
                              int32_t y) {
    // Unused.
}

static void
xdg_toplevel_unset_fullscreen(struct wl_client *client, struct wl_resource *resource) {
    struct server_xdg_toplevel *xdg_toplevel = wl_resource_get_user_data(resource);

    // This is a no-op, but we need to send a configure anyway.
    send_toplevel_configure(xdg_toplevel);
    server_xdg_surface_send_configure(xdg_toplevel->parent);
}

static void
xdg_toplevel_unset_maximized(struct wl_client *client, struct wl_resource *resource) {
    struct server_xdg_toplevel *xdg_toplevel = wl_resource_get_user_data(resource);

    // This is a no-op, but we need to send a configure anyway.
    send_toplevel_configure(xdg_toplevel);
    server_xdg_surface_send_configure(xdg_toplevel->parent);
}

static const struct xdg_toplevel_interface xdg_toplevel_impl = {
    .destroy = xdg_toplevel_destroy,
    .move = xdg_toplevel_move,
    .resize = xdg_toplevel_resize,
    .set_app_id = xdg_toplevel_set_app_id,
    .set_fullscreen = xdg_toplevel_set_fullscreen,
    .set_maximized = xdg_toplevel_set_maximized,
    .set_minimized = xdg_toplevel_set_minimized,
    .set_max_size = xdg_toplevel_set_max_size,
    .set_min_size = xdg_toplevel_set_min_size,
    .set_parent = xdg_toplevel_set_parent,
    .set_title = xdg_toplevel_set_title,
    .show_window_menu = xdg_toplevel_show_window_menu,
    .unset_fullscreen = xdg_toplevel_unset_fullscreen,
    .unset_maximized = xdg_toplevel_unset_maximized,
};

static void
xdg_surface_resource_destroy(struct wl_resource *resource) {
    struct server_xdg_surface *xdg_surface = wl_resource_get_user_data(resource);

    if (xdg_surface->child) {
        wl_resource_post_error(xdg_surface->resource, XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,
                               "xdg_surface destroyed before associated xdg_toplevel");
    }

    wl_list_remove(&xdg_surface->link);
    wl_list_remove(&xdg_surface->on_commit.link);
    wl_list_remove(&xdg_surface->on_destroy.link);

    free(xdg_surface);
}

static void
xdg_surface_ack_configure(struct wl_client *client, struct wl_resource *resource, uint32_t serial) {
    struct server_xdg_surface *xdg_surface = wl_resource_get_user_data(resource);

    if (serial_ring_consume(&xdg_surface->serials, serial) != 0) {
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SERIAL,
                               "invalid serial %" PRIu32 " given to xdg_surface.ack_configure",
                               serial);
        return;
    }

    xdg_surface->initial_ack = true;

    // TODO: ensure that window bounds have actually changed
}

static void
xdg_surface_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
xdg_surface_get_popup(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                      struct wl_resource *surface_resource,
                      struct wl_resource *positioner_resource) {
    // Unused.
    wl_client_post_implementation_error(client, "xdg_surface.get_popup is not supported");
}

static void
xdg_surface_get_toplevel(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_xdg_surface *xdg_surface = wl_resource_get_user_data(resource);

    switch (xdg_surface->parent->role) {
    case SURFACE_ROLE_NONE:
        break;
    case SURFACE_ROLE_CURSOR:
        wl_resource_post_error(xdg_surface->xdg_wm_base->resource, XDG_WM_BASE_ERROR_ROLE,
                               "cannot issue xdg_surface.get_toplevel on a surface with a role");
        return;
    case SURFACE_ROLE_XDG:
        if (xdg_surface->child) {
            wl_resource_post_error(xdg_surface->xdg_wm_base->resource, XDG_WM_BASE_ERROR_ROLE,
                                   "cannot have more than one xdg_toplevel per xdg_surface");
            return;
        }
        break;
    }

    struct server_xdg_toplevel *xdg_toplevel = calloc(1, sizeof(*xdg_toplevel));
    if (!xdg_toplevel) {
        ww_log(LOG_WARN, "failed to allocate server_xdg_toplevel");
        wl_resource_post_no_memory(resource);
        return;
    }

    xdg_toplevel->resource =
        wl_resource_create(client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
    ww_assert(xdg_toplevel->resource);
    wl_resource_set_implementation(xdg_toplevel->resource, &xdg_toplevel_impl, xdg_toplevel,
                                   xdg_toplevel_resource_destroy);
    wl_resource_set_user_data(xdg_toplevel->resource, xdg_toplevel);

    xdg_surface->parent->role = SURFACE_ROLE_XDG;
    xdg_surface->parent->role_object = xdg_toplevel;
    xdg_surface->child = xdg_toplevel;
    xdg_toplevel->parent = xdg_surface;

    wl_signal_init(&xdg_toplevel->events.destroy);
}

static void
xdg_surface_set_window_geometry(struct wl_client *client, struct wl_resource *resource, int32_t x,
                                int32_t y, int32_t width, int32_t height) {
    // Unused.
    wl_client_post_implementation_error(client, "xdg_surface.set_window_geometry is not supported");
}

static const struct xdg_surface_interface xdg_surface_impl = {
    .ack_configure = xdg_surface_ack_configure,
    .destroy = xdg_surface_destroy,
    .get_popup = xdg_surface_get_popup,
    .get_toplevel = xdg_surface_get_toplevel,
    .set_window_geometry = xdg_surface_set_window_geometry,
};

static void
xdg_wm_base_resource_destroy(struct wl_resource *resource) {
    struct server_xdg_wm_base *xdg_wm_base = wl_resource_get_user_data(resource);

    if (!wl_list_empty(&xdg_wm_base->surfaces)) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_DEFUNCT_SURFACES,
                               "xdg_wm_base destroyed with %d remaining surfaces",
                               wl_list_length(&xdg_wm_base->surfaces));
    }

    free(xdg_wm_base);
}

static void
xdg_wm_base_create_positioner(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    // Unused.
    wl_client_post_implementation_error(client, "xdg_wm_base.create_positioner is not supported");
}

static void
xdg_wm_base_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
xdg_wm_base_get_xdg_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                            struct wl_resource *surface_resource) {
    struct server_xdg_wm_base *xdg_wm_base = wl_resource_get_user_data(resource);
    struct server_surface *surface = server_surface_from_resource(surface_resource);

    if (surface->role != SURFACE_ROLE_NONE) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE,
                               "cannot create xdg_surface for wl_surface with a role");
        return;
    }
    if (surface->current.buffer || surface->pending.buffer) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE,
                               "cannot create xdg_surface for wl_surface with a buffer");
        return;
    }

    struct server_xdg_surface *xdg_surface = calloc(1, sizeof(*xdg_surface));
    if (!xdg_surface) {
        ww_log(LOG_WARN, "failed to allocate server_xdg_surface");
        wl_resource_post_no_memory(resource);
        return;
    }

    xdg_surface->resource =
        wl_resource_create(client, &xdg_surface_interface, wl_resource_get_version(resource), id);
    ww_assert(xdg_surface->resource);
    wl_resource_set_implementation(xdg_surface->resource, &xdg_surface_impl, xdg_surface,
                                   xdg_surface_resource_destroy);
    wl_resource_set_user_data(xdg_surface->resource, xdg_surface);

    wl_list_insert(&xdg_wm_base->surfaces, &xdg_surface->link);
    xdg_surface->xdg_wm_base = xdg_wm_base;
    xdg_surface->parent = surface;

    xdg_surface->on_commit.notify = on_surface_commit;
    wl_signal_add(&surface->events.commit, &xdg_surface->on_commit);

    xdg_surface->on_destroy.notify = on_surface_destroy;
    wl_signal_add(&surface->events.destroy, &xdg_surface->on_destroy);
}

static void
xdg_wm_base_pong(struct wl_client *client, struct wl_resource *resource, uint32_t serial) {
    wl_client_post_implementation_error(client, "xdg_wm_base.pong should not be sent");
}

static const struct xdg_wm_base_interface xdg_wm_base_impl = {
    .create_positioner = xdg_wm_base_create_positioner,
    .destroy = xdg_wm_base_destroy,
    .get_xdg_surface = xdg_wm_base_get_xdg_surface,
    .pong = xdg_wm_base_pong,
};

static void
on_global_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SRV_XDG_WM_BASE_VERSION);

    struct server_xdg_wm_base *xdg_wm_base = calloc(1, sizeof(*xdg_wm_base));
    if (!xdg_wm_base) {
        ww_log(LOG_WARN, "failed to allocate server_xdg_wm_base");
        wl_client_post_no_memory(client);
        return;
    }

    xdg_wm_base->resource = wl_resource_create(client, &xdg_wm_base_interface, version, id);
    ww_assert(xdg_wm_base->resource);
    wl_resource_set_implementation(xdg_wm_base->resource, &xdg_wm_base_impl, xdg_wm_base,
                                   xdg_wm_base_resource_destroy);
    wl_resource_set_user_data(xdg_wm_base->resource, xdg_wm_base);

    wl_list_init(&xdg_wm_base->surfaces);
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_xdg_wm_base_g *xdg_wm_base_g =
        wl_container_of(listener, xdg_wm_base_g, on_display_destroy);

    wl_global_destroy(xdg_wm_base_g->global);

    wl_list_remove(&xdg_wm_base_g->on_display_destroy.link);

    free(xdg_wm_base_g);
}

struct server_xdg_wm_base_g *
server_xdg_wm_base_g_create(struct server *server) {
    struct server_xdg_wm_base_g *xdg_wm_base_g = calloc(1, sizeof(*xdg_wm_base_g));
    if (!xdg_wm_base_g) {
        ww_log(LOG_ERROR, "failed to allocate server_xdg_wm_base_g");
        return NULL;
    }

    xdg_wm_base_g->global =
        wl_global_create(server->display, &xdg_wm_base_interface, SRV_XDG_WM_BASE_VERSION,
                         xdg_wm_base_g, on_global_bind);
    ww_assert(xdg_wm_base_g->global);

    xdg_wm_base_g->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &xdg_wm_base_g->on_display_destroy);

    return xdg_wm_base_g;
}

void
server_xdg_surface_send_configure(struct server_xdg_surface *xdg_surface) {
    uint32_t serial = next_serial(xdg_surface->resource);
    if (serial_ring_push(&xdg_surface->serials, serial) != 0) {
        wl_resource_post_no_memory(xdg_surface->resource);
        return;
    }
    xdg_surface_send_configure(xdg_surface->resource, serial);
}

struct server_xdg_surface *
server_xdg_surface_from_resource(struct wl_resource *resource) {
    ww_assert(wl_resource_instance_of(resource, &xdg_surface_interface, &xdg_surface_impl));
    return wl_resource_get_user_data(resource);
}

struct server_xdg_toplevel *
server_xdg_toplevel_from_resource(struct wl_resource *resource) {
    ww_assert(wl_resource_instance_of(resource, &xdg_toplevel_interface, &xdg_toplevel_impl));
    return wl_resource_get_user_data(resource);
}