#include "compositor/xdg_shell.h"
#include "compositor/buffer.h"
#include "compositor/server.h"
#include "compositor/wl_compositor.h"
#include "util.h"
#include <wayland-client.h>
#include <wayland-server.h>

#include "xdg-shell-server-protocol.h"

#define VERSION 6

struct client_xdg_wm_base {
    struct wl_list surfaces;
};

static const struct xdg_wm_base_interface xdg_wm_base_impl;

static struct client_xdg_wm_base *
client_xdg_wm_base_from_resource(struct wl_resource *resource) {
    ww_assert(wl_resource_instance_of(resource, &xdg_wm_base_interface, &xdg_wm_base_impl));
    return wl_resource_get_user_data(resource);
}

static void
send_toplevel_configure(struct server_xdg_toplevel *xdg_toplevel) {
    struct wl_array states;

    wl_array_init(&states);

    if (xdg_toplevel->fullscreen) {
        uint32_t *state = wl_array_add(&states, sizeof(uint32_t));
        *state = XDG_TOPLEVEL_STATE_FULLSCREEN;
    }

    xdg_toplevel_send_configure(xdg_toplevel->resource, xdg_toplevel->width, xdg_toplevel->height,
                                &states);
    server_xdg_surface_send_configure(xdg_toplevel->parent);
    wl_array_release(&states);
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_xdg_wm_base *xdg_wm_base =
        wl_container_of(listener, xdg_wm_base, display_destroy);

    free(xdg_wm_base);
}

static void
on_surface_commit(struct wl_listener *listener, void *data) {
    struct server_xdg_surface *xdg_surface = wl_container_of(listener, xdg_surface, on_commit);
    struct server_surface *surface = data;

    if (!xdg_surface->configured) {
        if (surface->pending.buffer) {
            wl_resource_post_error(xdg_surface->resource, XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
                                   "wl_surface.commit was called with an attached buffer for an "
                                   "unconfigured xdg_surface");
            return;
        }

        server_xdg_surface_send_configure(xdg_surface);
    }
}

static void
on_surface_destroy(struct wl_listener *listener, void *data) {
    struct server_xdg_surface *xdg_surface = wl_container_of(listener, xdg_surface, on_destroy);
    struct server_surface *surface = data;

    wl_resource_post_error(surface->resource, WL_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,
                           "wl_surface destroyed before attached xdg_surface");
}

static void
handle_xdg_toplevel_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_xdg_toplevel_set_parent(struct wl_client *client, struct wl_resource *resource,
                               struct wl_resource *parent_resource) {
    // No relevant clients make use of this function.
    wl_client_post_implementation_error(client, "xdg_toplevel.set_parent is not implemented");
}

static void
handle_xdg_toplevel_set_title(struct wl_client *client, struct wl_resource *resource,
                              const char *title) {
    struct server_xdg_toplevel *xdg_toplevel = server_xdg_toplevel_from_resource(resource);

    if (xdg_toplevel->title) {
        free(xdg_toplevel->title);
    }
    xdg_toplevel->title = strdup(title);

    wl_signal_emit_mutable(&xdg_toplevel->events.set_title, xdg_toplevel->title);
}

static void
handle_xdg_toplevel_set_app_id(struct wl_client *client, struct wl_resource *resource,
                               const char *app_id) {
    // No relevant clients make use of this function.
    // Unused.
}

static void
handle_xdg_toplevel_show_window_menu(struct wl_client *client, struct wl_resource *resource,
                                     struct wl_resource *seat_resource, uint32_t serial, int32_t x,
                                     int32_t y) {
    // No relevant clients make use of this function.
    wl_client_post_implementation_error(client, "xdg_toplevel.show_window_menu is not implemented");
}

static void
handle_xdg_toplevel_move(struct wl_client *client, struct wl_resource *resource,
                         struct wl_resource *seat_resource, uint32_t serial) {
    // No relevant clients make use of this function.
    wl_client_post_implementation_error(client, "xdg_toplevel.move is not implemented");
}

static void
handle_xdg_toplevel_resize(struct wl_client *client, struct wl_resource *resource,
                           struct wl_resource *seat_resource, uint32_t serial, uint32_t edges) {
    // No relevant clients make use of this function.
    wl_client_post_implementation_error(client, "xdg_toplevel.resize is not implemented");
}

static void
handle_xdg_toplevel_set_max_size(struct wl_client *client, struct wl_resource *resource,
                                 int32_t width, int32_t height) {
    if (width < 0 || height < 0) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_SIZE,
                               "invalid size provided to xdg_toplevel.set_max_size (%" PRIu32
                               "x%" PRIu32 ")",
                               width, height);
    }

    // We do not care about what the client wants.
}

static void
handle_xdg_toplevel_set_min_size(struct wl_client *client, struct wl_resource *resource,
                                 int32_t width, int32_t height) {
    if (width < 0 || height < 0) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_SIZE,
                               "invalid size provided to xdg_toplevel.set_min_size (%" PRIu32
                               "x%" PRIu32 ")",
                               width, height);
    }

    // We do not care about what the client wants.
}

static void
handle_xdg_toplevel_set_maximized(struct wl_client *client, struct wl_resource *resource) {
    struct server_xdg_toplevel *xdg_toplevel = server_xdg_toplevel_from_resource(resource);

    // We do not care about maximization. Tell the client they are not maximized.
    send_toplevel_configure(xdg_toplevel);
}

static void
handle_xdg_toplevel_unset_maximized(struct wl_client *client, struct wl_resource *resource) {
    struct server_xdg_toplevel *xdg_toplevel = server_xdg_toplevel_from_resource(resource);

    // We do not care about maximization. Tell the client they are not maximized.
    send_toplevel_configure(xdg_toplevel);
}

static void
handle_xdg_toplevel_set_fullscreen(struct wl_client *client, struct wl_resource *resource,
                                   struct wl_resource *output_resource) {
    struct server_xdg_toplevel *xdg_toplevel = server_xdg_toplevel_from_resource(resource);

    // It is up to whoever is listening to this event to decide if the surface should be
    // fullscreened. They can set the `fullscreen` field on the server_xdg_toplevel.
    wl_signal_emit_mutable(&xdg_toplevel->events.set_fullscreen, xdg_toplevel);
    send_toplevel_configure(xdg_toplevel);
}

static void
handle_xdg_toplevel_unset_fullscreen(struct wl_client *client, struct wl_resource *resource) {
    struct server_xdg_toplevel *xdg_toplevel = server_xdg_toplevel_from_resource(resource);

    xdg_toplevel->fullscreen = false;
    wl_signal_emit_mutable(&xdg_toplevel->events.unset_fullscreen, xdg_toplevel);
    send_toplevel_configure(xdg_toplevel);
}

static void
handle_xdg_toplevel_set_minimized(struct wl_client *client, struct wl_resource *resource) {
    // Unused.
}

static void
xdg_toplevel_destroy(struct wl_resource *resource) {
    struct server_xdg_toplevel *xdg_toplevel = server_xdg_toplevel_from_resource(resource);

    wl_signal_emit_mutable(&xdg_toplevel->events.destroy, xdg_toplevel);

    if (xdg_toplevel->parent) {
        xdg_toplevel->parent->toplevel = NULL;

        ww_assert(xdg_toplevel->parent->parent->role == ROLE_XDG_TOPLEVEL);
        xdg_toplevel->parent->parent->role_object = NULL;
    }

    if (xdg_toplevel->title) {
        free(xdg_toplevel->title);
    }

    free(xdg_toplevel);
}

static const struct xdg_toplevel_interface xdg_toplevel_impl = {
    .destroy = handle_xdg_toplevel_destroy,
    .set_parent = handle_xdg_toplevel_set_parent,
    .set_title = handle_xdg_toplevel_set_title,
    .set_app_id = handle_xdg_toplevel_set_app_id,
    .show_window_menu = handle_xdg_toplevel_show_window_menu,
    .move = handle_xdg_toplevel_move,
    .resize = handle_xdg_toplevel_resize,
    .set_max_size = handle_xdg_toplevel_set_max_size,
    .set_min_size = handle_xdg_toplevel_set_min_size,
    .set_maximized = handle_xdg_toplevel_set_maximized,
    .unset_maximized = handle_xdg_toplevel_unset_maximized,
    .set_fullscreen = handle_xdg_toplevel_set_fullscreen,
    .unset_fullscreen = handle_xdg_toplevel_unset_fullscreen,
    .set_minimized = handle_xdg_toplevel_set_minimized,
};

static void
handle_xdg_surface_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_xdg_surface_get_toplevel(struct wl_client *client, struct wl_resource *resource,
                                uint32_t id) {
    struct server_xdg_surface *xdg_surface = server_xdg_surface_from_resource(resource);

    struct wl_resource *xdg_toplevel_resource =
        wl_resource_create(client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
    if (!xdg_toplevel_resource) {
        wl_client_post_no_memory(client);
        return;
    }

    if (xdg_surface->toplevel) {
        // XXX: Is this the "correct" error code? I cannot find any codebases elsewhere which make
        // use of this, but I can't find anything better either.
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
                               "xdg_surface cannot have more than one toplevel");
        return;
    }

    enum server_surface_role role = xdg_surface->parent->role;
    ww_assert(role == ROLE_XDG_SURFACE || role == ROLE_XDG_TOPLEVEL);

    struct server_xdg_toplevel *xdg_toplevel = calloc(1, sizeof(*xdg_toplevel));
    if (!xdg_toplevel) {
        wl_client_post_no_memory(client);
        return;
    }

    wl_signal_init(&xdg_toplevel->events.destroy);
    wl_signal_init(&xdg_toplevel->events.unset_fullscreen);
    wl_signal_init(&xdg_toplevel->events.set_fullscreen);
    wl_signal_init(&xdg_toplevel->events.set_title);

    wl_resource_set_implementation(xdg_toplevel_resource, &xdg_toplevel_impl, xdg_toplevel,
                                   xdg_toplevel_destroy);

    xdg_toplevel->parent = xdg_surface;
    xdg_toplevel->resource = xdg_toplevel_resource;
    xdg_surface->parent->role = ROLE_XDG_TOPLEVEL;
    xdg_surface->parent->role_object = xdg_toplevel->resource;
}

static void
handle_xdg_surface_get_popup(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                             struct wl_resource *xdg_surface_resource,
                             struct wl_resource *positioner_resource) {
    // No relevant clients make use of this function.
    wl_client_post_implementation_error(client, "xdg_surface.get_popup is not implemented");
}

static void
handle_xdg_surface_set_window_geometry(struct wl_client *client, struct wl_resource *resource,
                                       int32_t x, int32_t y, int32_t width, int32_t height) {
    struct server_xdg_surface *xdg_surface = server_xdg_surface_from_resource(resource);

    if (!xdg_surface->toplevel) {
        wl_resource_post_error(
            resource, XDG_SURFACE_ERROR_NOT_CONSTRUCTED,
            "xdg_surface.set_window_geometry called before a toplevel was created");
        return;
    }

    if (width <= 0 || height <= 0) {
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SIZE,
                               "xdg_surface.set_window_geometry called with invalid size (%" PRIu32
                               "x%" PRIu32 ")",
                               width, height);
        return;
    }

    // No relevant clients make use of this function.
}

static void
handle_xdg_surface_ack_configure(struct wl_client *client, struct wl_resource *resource,
                                 uint32_t serial) {
    struct server_xdg_surface *xdg_surface = server_xdg_surface_from_resource(resource);

    struct ringbuf *ringbuf = &xdg_surface->configure_serials;
    for (size_t i = 0; i < ringbuf->count; i++) {
        uint32_t buf_serial = ringbuf_at(ringbuf, i);

        if (buf_serial == serial) {
            ringbuf_shift(ringbuf, i + 1);
            xdg_surface->configured = true;
            return;
        }
    }

    wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SERIAL,
                           "xdg_surface.configure received serial %" PRIu32 " which was never sent",
                           serial);
}

static void
xdg_surface_destroy(struct wl_resource *resource) {
    struct server_xdg_surface *xdg_surface = server_xdg_surface_from_resource(resource);

    if (xdg_surface->toplevel) {
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,
                               "xdg_surface was destroyed before its associated xdg_toplevel");
    }

    xdg_surface->toplevel->parent = NULL;
    if (xdg_surface->parent->role == ROLE_XDG_SURFACE) {
        xdg_surface->parent->role_object = NULL;
    }

    wl_list_remove(&xdg_surface->on_commit.link);
    wl_list_remove(&xdg_surface->on_destroy.link);

    wl_list_remove(&xdg_surface->link);
    free(xdg_surface);
}

static const struct xdg_surface_interface xdg_surface_impl = {
    .destroy = handle_xdg_surface_destroy,
    .get_toplevel = handle_xdg_surface_get_toplevel,
    .get_popup = handle_xdg_surface_get_popup,
    .set_window_geometry = handle_xdg_surface_set_window_geometry,
    .ack_configure = handle_xdg_surface_ack_configure,
};

static void
handle_xdg_wm_base_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_xdg_wm_base_create_positioner(struct wl_client *client, struct wl_resource *resource,
                                     uint32_t id) {
    // No relevant clients make use of this function.
    wl_client_post_implementation_error(client, "xdg_wm_base.create_positioner is not implemented");
}

static void
handle_xdg_wm_base_get_xdg_surface(struct wl_client *client, struct wl_resource *resource,
                                   uint32_t id, struct wl_resource *surface_resource) {
    struct client_xdg_wm_base *xdg_wm_base = client_xdg_wm_base_from_resource(resource);
    struct server_surface *surface = server_surface_from_resource(surface_resource);

    struct wl_resource *xdg_surface_resource =
        wl_resource_create(client, &xdg_surface_interface, wl_resource_get_version(resource), id);
    if (!xdg_surface_resource) {
        wl_client_post_no_memory(client);
        return;
    }

    if (surface->role != ROLE_NONE && surface->role != ROLE_XDG_SURFACE &&
        surface->role != ROLE_XDG_TOPLEVEL) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE,
                               "surface given to xdg_wm_base.get_xdg_surface already has a role");
        return;
    }
    if (surface->role_object) {
        wl_resource_post_error(
            resource, XDG_WM_BASE_ERROR_ROLE,
            "surface given to xdg_wm_base.get_xdg_surface already has a role object");
        return;
    }
    if (surface->pending.buffer) {
        wl_resource_post_error(
            resource, XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE,
            "surface given to xdg_wm_base.get_xdg_surface has a buffer attached");
        return;
    }
    if (surface->current_buffer) {
        wl_resource_post_error(
            resource, XDG_WM_BASE_ERROR_INVALID_SURFACE_STATE,
            "surface given to xdg_wm_base.get_xdg_surface has a buffer committed");
        return;
    }

    struct server_xdg_surface *xdg_surface = calloc(1, sizeof(*xdg_surface));
    if (!xdg_surface) {
        wl_client_post_no_memory(client);
        return;
    }

    wl_resource_set_implementation(xdg_surface_resource, &xdg_surface_impl, xdg_surface,
                                   xdg_surface_destroy);

    xdg_surface->on_commit.notify = on_surface_commit;
    wl_signal_add(&surface->events.commit, &xdg_surface->on_commit);
    xdg_surface->on_destroy.notify = on_surface_destroy;
    wl_signal_add(&surface->events.destroy, &xdg_surface->on_destroy);

    wl_list_insert(&xdg_wm_base->surfaces, &xdg_surface->link);

    surface->role = ROLE_XDG_SURFACE;
    surface->role_object = xdg_surface_resource;
}

static void
handle_xdg_wm_base_pong(struct wl_client *client, struct wl_resource *resource, uint32_t serial) {
    // We never send a ping event, so there should be nothing using this request.
    // TODO: Maybe we want to use this for instance crash detection
    wl_client_post_implementation_error(client,
                                        "xdg_wm_base.pong received when no ping event was sent");
}

static void
xdg_wm_base_destroy(struct wl_resource *resource) {
    struct client_xdg_wm_base *xdg_wm_base = client_xdg_wm_base_from_resource(resource);

    int num_surfaces = wl_list_length(&xdg_wm_base->surfaces);
    if (num_surfaces > 0) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_DEFUNCT_SURFACES,
                               "%d alive surfaces owned by destroyed xdg_wm_base", num_surfaces);
    }

    free(xdg_wm_base);
}

static const struct xdg_wm_base_interface xdg_wm_base_impl = {
    .destroy = handle_xdg_wm_base_destroy,
    .create_positioner = handle_xdg_wm_base_create_positioner,
    .get_xdg_surface = handle_xdg_wm_base_get_xdg_surface,
    .pong = handle_xdg_wm_base_pong,
};

static void
handle_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= VERSION);

    struct wl_resource *resource = wl_resource_create(client, &xdg_wm_base_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }

    struct client_xdg_wm_base *client_xdg_wm_base = calloc(1, sizeof(*client_xdg_wm_base));
    if (!client_xdg_wm_base) {
        wl_client_post_no_memory(client);
        return;
    }

    wl_list_init(&client_xdg_wm_base->surfaces);

    wl_resource_set_implementation(resource, &xdg_wm_base_impl, client_xdg_wm_base,
                                   xdg_wm_base_destroy);
}

struct server_xdg_wm_base *
server_xdg_wm_base_create(struct server *server) {
    struct server_xdg_wm_base *xdg_wm_base = calloc(1, sizeof(*xdg_wm_base));
    if (!xdg_wm_base) {
        LOG(LOG_ERROR, "failed to allocate server_xdg_wm_base");
        return NULL;
    }

    xdg_wm_base->global = wl_global_create(server->display, &xdg_wm_base_interface, VERSION,
                                           xdg_wm_base, handle_bind);

    xdg_wm_base->display_destroy.notify = on_display_destroy;

    wl_display_add_destroy_listener(server->display, &xdg_wm_base->display_destroy);

    return xdg_wm_base;
}

void
server_xdg_surface_send_configure(struct server_xdg_surface *xdg_surface) {
    struct ringbuf *ringbuf = &xdg_surface->configure_serials;

    if (ringbuf->count == RINGBUF_SIZE) {
        wl_client_post_implementation_error(
            wl_resource_get_client(xdg_surface->resource),
            "xdg_surface has not responded to the past %d configure requests", RINGBUF_SIZE);
        return;
    }

    ringbuf_push(ringbuf, next_serial(xdg_surface->resource));
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
