#include "server/xwayland_shell.h"
#include "server/server.h"
#include "server/wl_compositor.h"
#include "util/alloc.h"
#include "util/prelude.h"
#include "xwayland-shell-v1-server-protocol.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#define SRV_XWAYLAND_SHELL_VERSION 1

static void
xwayland_surface_role_commit(struct wl_resource *role_resource) {
    struct server_xwayland_surface *xwayland_surface = wl_resource_get_user_data(role_resource);

    if (xwayland_surface->associated || !xwayland_surface->pending_association) {
        return;
    }

    xwayland_surface->pending_association = false;
    xwayland_surface->associated = true;
    wl_signal_emit(&xwayland_surface->events.set_serial, &xwayland_surface->pending_serial);
}

static void
xwayland_surface_role_destroy(struct wl_resource *role_resource) {
    // Unused.
}

static const struct server_surface_role xwayland_surface_role = {
    .name = "xwayland_surface",

    .commit = xwayland_surface_role_commit,
    .destroy = xwayland_surface_role_destroy,
};

static void
xwayland_surface_resource_destroy(struct wl_resource *resource) {
    struct server_xwayland_surface *xwayland_surface = wl_resource_get_user_data(resource);

    wl_signal_emit(&xwayland_surface->events.destroy, NULL);
    free(xwayland_surface);
}

static void
xwayland_surface_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
xwayland_surface_set_serial(struct wl_client *client, struct wl_resource *resource,
                            uint32_t serial_lo, uint32_t serial_hi) {
    struct server_xwayland_surface *xwayland_surface = wl_resource_get_user_data(resource);
    uint64_t serial = (uint64_t)serial_lo | ((uint64_t)serial_hi) << 32;

    if (xwayland_surface->associated) {
        wl_resource_post_error(resource, XWAYLAND_SURFACE_V1_ERROR_ALREADY_ASSOCIATED,
                               "xwayland_surface was already associated with an X11 window");
        return;
    }

    xwayland_surface->pending_association = true;
    xwayland_surface->pending_serial = serial;
}

static const struct xwayland_surface_v1_interface xwayland_surface_impl = {
    .destroy = xwayland_surface_destroy,
    .set_serial = xwayland_surface_set_serial,
};

static void
xwayland_shell_resource_destroy(struct wl_resource *resource) {
    struct server_xwayland_shell *xwayland_shell = wl_resource_get_user_data(resource);

    ww_assert(xwayland_shell->resource == resource);

    xwayland_shell->resource = NULL;
    xwayland_shell->bound = false;
}

static void
xwayland_shell_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
xwayland_shell_get_xwayland_surface(struct wl_client *client, struct wl_resource *resource,
                                    uint32_t id, struct wl_resource *surface_resource) {
    struct server_xwayland_shell *xwayland_shell = wl_resource_get_user_data(resource);
    struct server_surface *surface = server_surface_from_resource(surface_resource);

    struct server_xwayland_surface *xwayland_surface = zalloc(1, sizeof(*xwayland_surface));

    struct wl_resource *xwayland_surface_resource = wl_resource_create(
        client, &xwayland_surface_v1_interface, wl_resource_get_version(resource), id);
    check_alloc(resource);
    wl_resource_set_implementation(xwayland_surface_resource, &xwayland_surface_impl,
                                   xwayland_surface, xwayland_surface_resource_destroy);

    xwayland_surface->resource = xwayland_surface_resource;
    xwayland_surface->parent = surface;

    wl_signal_init(&xwayland_surface->events.destroy);
    wl_signal_init(&xwayland_surface->events.set_serial);

    if (server_surface_set_role(xwayland_surface->parent, &xwayland_surface_role,
                                xwayland_surface->resource) != 0) {
        wl_resource_post_error(xwayland_shell->resource, XWAYLAND_SHELL_V1_ERROR_ROLE,
                               "cannot create xwayland_surface for surface with another role");
        wl_resource_destroy(xwayland_surface->resource);
        return;
    }

    wl_signal_emit(&xwayland_shell->events.new_surface, xwayland_surface);
}

static const struct xwayland_shell_v1_interface xwayland_shell_impl = {
    .destroy = xwayland_shell_destroy,
    .get_xwayland_surface = xwayland_shell_get_xwayland_surface,
};

static void
on_global_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SRV_XWAYLAND_SHELL_VERSION);

    struct server_xwayland_shell *xwayland_shell = data;

    if (xwayland_shell->resource) {
        wl_client_post_implementation_error(client, "xwayland_shell was already bound");
        return;
    }

    struct wl_resource *resource =
        wl_resource_create(client, &xwayland_shell_v1_interface, version, id);
    check_alloc(resource);
    wl_resource_set_implementation(resource, &xwayland_shell_impl, xwayland_shell,
                                   xwayland_shell_resource_destroy);

    xwayland_shell->resource = resource;
    xwayland_shell->bound = true;
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_xwayland_shell *xwayland_shell =
        wl_container_of(listener, xwayland_shell, on_display_destroy);

    wl_global_destroy(xwayland_shell->global);

    wl_list_remove(&xwayland_shell->on_display_destroy.link);

    free(xwayland_shell);
}

struct server_xwayland_shell *
server_xwayland_shell_create(struct server *server) {
    struct server_xwayland_shell *xwayland_shell = zalloc(1, sizeof(*xwayland_shell));

    xwayland_shell->global =
        wl_global_create(server->display, &xwayland_shell_v1_interface, SRV_XWAYLAND_SHELL_VERSION,
                         xwayland_shell, on_global_bind);
    check_alloc(xwayland_shell->global);

    xwayland_shell->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &xwayland_shell->on_display_destroy);

    wl_signal_init(&xwayland_shell->events.new_surface);

    return xwayland_shell;
}
