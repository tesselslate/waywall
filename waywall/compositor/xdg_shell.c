#include "compositor/xdg_shell.h"
#include "compositor/buffer.h"
#include "compositor/server.h"
#include "compositor/wl_compositor.h"
#include "util.h"
#include <wayland-client.h>
#include <wayland-server.h>

#include "xdg-shell-server-protocol.h"

#define VERSION 6

// TODO: wl_surface destroyed before xdg_surface can probably cause a crash
// TODO: theres a bunch of annoying logic where we're supposed to kill the client if they do stuff
// with the surface before it's ready for use but I don't know that it really matters

struct client_xdg_wm_base {
    struct wl_resource *resource;

    uint32_t surface_count;
};

static void
send_surface_configure(struct server_xdg_surface *xdg_surface) {
    struct ringbuf *serials = &xdg_surface->configure_serials;

    if (serials->count == RINGBUF_SIZE) {
        wl_client_post_implementation_error(
            wl_resource_get_client(xdg_surface->resource),
            "too many queued xdg_surface.configure events have not been acked");
        return;
    }

    uint32_t serial = next_serial(xdg_surface->resource);
    bool ok = ringbuf_push(serials, serial);
    ww_assert(ok);

    xdg_surface_send_configure(xdg_surface->resource, serial);
}

static void
send_toplevel_configure(struct server_xdg_toplevel *xdg_toplevel) {
    struct wl_array states;
    wl_array_init(&states);
    if (xdg_toplevel->pending.fullscreen) {
        uint32_t *state = wl_array_add(&states, sizeof(uint32_t));
        *state = XDG_TOPLEVEL_STATE_FULLSCREEN;
    }

    xdg_toplevel_send_configure(xdg_toplevel->resource, xdg_toplevel->pending.width,
                                xdg_toplevel->pending.height, &states);
    wl_array_release(&states);
}

static void
on_surface_commit(struct wl_listener *listener, void *data) {
    struct server_xdg_surface *xdg_surface = wl_container_of(listener, xdg_surface, on_commit);
    struct server_surface *surface = data;

    ww_assert(xdg_surface->surface == surface);

    // TODO: we can complain if the client hasnt properly configured the xdg_surface yet here

    if (!xdg_surface->toplevel) {
        return;
    }

    struct server_buffer *current_buffer =
        surface->pending_buffer_changed ? surface->pending_buffer : surface->current_buffer;
    uint32_t current_width = server_buffer_get_width(current_buffer);
    uint32_t current_height = server_buffer_get_height(current_buffer);

    if (xdg_surface->toplevel->pending.width != current_width ||
        xdg_surface->toplevel->pending.height != current_height) {
        // The client did not listen to the latest configure request, so give them another.
        // TODO: kill the client if they continuously do not obey the configure request
        send_toplevel_configure(xdg_surface->toplevel);
        send_surface_configure(xdg_surface);
    }
    xdg_surface->toplevel->current = xdg_surface->toplevel->pending;
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_xdg_wm_base *xdg_wm_base =
        wl_container_of(listener, xdg_wm_base, display_destroy);

    free(xdg_wm_base);
}

static void
handle_xdg_toplevel_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_xdg_toplevel_set_parent(struct wl_client *client, struct wl_resource *resource,
                               struct wl_resource *xdg_toplevel_resource) {
    // No relevant clients make use of this function.
    wl_client_post_implementation_error(client, "xdg_toplevel.set_parent is not implemented");
}

static void
handle_xdg_toplevel_set_title(struct wl_client *client, struct wl_resource *resource,
                              const char *title) {
    struct server_xdg_toplevel *xdg_toplevel = wl_resource_get_user_data(resource);

    if (xdg_toplevel->title) {
        free(xdg_toplevel->title);
    }
    xdg_toplevel->title = strdup(title);
}

static void
handle_xdg_toplevel_set_app_id(struct wl_client *client, struct wl_resource *resource,
                               const char *app_id) {
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
    // Unused.
}

static void
handle_xdg_toplevel_set_min_size(struct wl_client *client, struct wl_resource *resource,
                                 int32_t width, int32_t height) {
    // Unused.
}

static void
handle_xdg_toplevel_set_maximized(struct wl_client *client, struct wl_resource *resource) {
    struct server_xdg_toplevel *xdg_toplevel = wl_resource_get_user_data(resource);

    if (xdg_toplevel->current.fullscreen) {
        return;
    }

    // We do not care about maximization and will tell the client they are not maximized.
    send_toplevel_configure(xdg_toplevel);
    send_surface_configure(xdg_toplevel->parent);
}

static void
handle_xdg_toplevel_unset_maximized(struct wl_client *client, struct wl_resource *resource) {
    struct server_xdg_toplevel *xdg_toplevel = wl_resource_get_user_data(resource);

    if (xdg_toplevel->current.fullscreen) {
        return;
    }

    // We do not care about maximization and will tell the client they are not maximized.
    send_toplevel_configure(xdg_toplevel);
    send_surface_configure(xdg_toplevel->parent);
}

static void
handle_xdg_toplevel_set_fullscreen(struct wl_client *client, struct wl_resource *resource,
                                   struct wl_resource *output_resource) {
    struct server_xdg_toplevel *xdg_toplevel = wl_resource_get_user_data(resource);

    // TODO: do not allow fullscreen on wall (queue it?)

    xdg_toplevel->pending.fullscreen = true;

    send_toplevel_configure(xdg_toplevel);
    send_surface_configure(xdg_toplevel->parent);
}

static void
handle_xdg_toplevel_unset_fullscreen(struct wl_client *client, struct wl_resource *resource) {
    struct server_xdg_toplevel *xdg_toplevel = wl_resource_get_user_data(resource);

    xdg_toplevel->pending.fullscreen = false;

    send_toplevel_configure(xdg_toplevel);
    send_surface_configure(xdg_toplevel->parent);
}

static void
handle_xdg_toplevel_set_minimized(struct wl_client *client, struct wl_resource *resource) {
    // Do nothing.
}

static void
xdg_toplevel_destroy(struct wl_resource *resource) {
    struct server_xdg_toplevel *xdg_toplevel = wl_resource_get_user_data(resource);

    xdg_toplevel->parent->toplevel = NULL;
    xdg_toplevel->parent->surface->role_object = NULL;

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
    struct server_xdg_surface *xdg_surface = wl_resource_get_user_data(resource);

    if (xdg_surface->toplevel) {
        wl_client_post_implementation_error(client,
                                            "xdg_toplevel already created for this xdg_surface");
        return;
    }
    if (xdg_surface->surface->role != ROLE_NONE) {
        wl_client_post_implementation_error(client,
                                            "xdg_surface's parent was given role object already");
        return;
    }

    struct server_xdg_toplevel *xdg_toplevel = calloc(1, sizeof(*xdg_toplevel));
    if (!xdg_toplevel) {
        wl_client_post_no_memory(client);
        return;
    }

    struct wl_resource *xdg_toplevel_resource =
        wl_resource_create(client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(xdg_toplevel_resource, &xdg_toplevel_impl, xdg_toplevel,
                                   xdg_toplevel_destroy);

    struct wl_array caps;
    wl_array_init(&caps);
    uint32_t *cap = wl_array_add(&caps, sizeof(uint32_t));
    *cap = XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN;
    xdg_toplevel_send_wm_capabilities(xdg_toplevel_resource, &caps);
    wl_array_release(&caps);

    xdg_toplevel->parent = xdg_surface;
    xdg_toplevel->resource = xdg_toplevel_resource;
    xdg_surface->toplevel = xdg_toplevel;
    xdg_surface->surface->role = ROLE_TOPLEVEL;
    xdg_surface->surface->role_object = xdg_toplevel_resource;
}

static void
handle_xdg_surface_get_popup(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                             struct wl_resource *xdg_surface_resource,
                             struct wl_resource *xdg_positioner_resource) {
    // No relevant clients make use of this function.
    wl_client_post_implementation_error(client, "xdg_surface.get_popup is not implemented");
}

static void
handle_xdg_surface_set_window_geometry(struct wl_client *client, struct wl_resource *resource,
                                       int32_t x, int32_t y, int32_t width, int32_t height) {
    // No relevant clients make use of this function.
    wl_client_post_implementation_error(client,
                                        "xdg_surface.set_window_geometry is not implemented");
}

static void
handle_xdg_surface_ack_configure(struct wl_client *client, struct wl_resource *resource,
                                 uint32_t serial) {
    struct server_xdg_surface *xdg_surface = wl_resource_get_user_data(resource);

    if (!xdg_surface->toplevel) {
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_NOT_CONSTRUCTED,
                               "xdg_surface.ack_configure called before toplevel created");
        return;
    }

    struct ringbuf *serials = &xdg_surface->configure_serials;
    for (size_t i = 0; i < serials->count; i++) {
        if (ringbuf_at(serials, i) == serial) {
            ringbuf_shift(serials, i + 1);
            return;
        }
    }

    wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SERIAL, "invalid serial %" PRIu32,
                           serial);
}

static void
xdg_surface_destroy(struct wl_resource *resource) {
    struct server_xdg_surface *xdg_surface = wl_resource_get_user_data(resource);

    if (xdg_surface->toplevel) {
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,
                               "destroyed xdg_surface before associated xdg_toplevel");
        wl_resource_destroy(xdg_surface->toplevel->resource);
    }

    xdg_surface->surface->role_object = NULL;
    xdg_surface->parent->surface_count--;

    wl_list_remove(&xdg_surface->on_commit.link);

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
    struct client_xdg_wm_base *client_xdg_wm_base = wl_resource_get_user_data(resource);
    if (client_xdg_wm_base->surface_count > 0) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_DEFUNCT_SURFACES,
                               "%" PRIu32 " alive surfaces owned by destroyed xdg_wm_base",
                               client_xdg_wm_base->surface_count);
    }

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
    struct client_xdg_wm_base *client_xdg_wm_base = wl_resource_get_user_data(resource);
    struct server_surface *surface = wl_resource_get_user_data(surface_resource);

    if (surface->role != ROLE_NONE) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE,
                               "surface given to xdg_wm_base.get_xdg_surface already has a role");
        return;
    }

    if (surface->current_buffer || surface->pending_buffer) {
        wl_client_post_implementation_error(
            client, "buffer already attached to wl_surface on xdg_wm_base.get_xdg_surface");
        return;
    }

    struct server_xdg_surface *xdg_surface = calloc(1, sizeof(*xdg_surface));
    if (!xdg_surface) {
        wl_client_post_no_memory(client);
        return;
    }
    xdg_surface->parent = client_xdg_wm_base;
    xdg_surface->surface = surface;

    struct wl_resource *xdg_surface_resource =
        wl_resource_create(client, &xdg_surface_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(xdg_surface_resource, &xdg_surface_impl, xdg_surface,
                                   xdg_surface_destroy);
    xdg_surface->resource = xdg_surface_resource;

    client_xdg_wm_base->surface_count++;

    xdg_surface->on_commit.notify = on_surface_commit;
    wl_signal_add(&surface->events.commit, &xdg_surface->on_commit);
}

static void
handle_xdg_wm_base_pong(struct wl_client *client, struct wl_resource *resource, uint32_t serial) {
    // TODO: we can use this later to detect instance crashes and kill them i guess
}

static void
xdg_wm_base_destroy(struct wl_resource *resource) {
    struct client_xdg_wm_base *client_xdg_wm_base = wl_resource_get_user_data(resource);
    free(client_xdg_wm_base);
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

    struct client_xdg_wm_base *client_xdg_wm_base = calloc(1, sizeof(*client_xdg_wm_base));
    if (!client_xdg_wm_base) {
        wl_client_post_no_memory(client);
        return;
    }

    struct wl_resource *resource = wl_resource_create(client, &xdg_wm_base_interface, version, id);
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
