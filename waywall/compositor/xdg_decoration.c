#include "compositor/xdg_decoration.h"
#include "compositor/server.h"
#include "compositor/wl_compositor.h"
#include "compositor/xdg_shell.h"
#include "util.h"
#include "xdg-decoration-unstable-v1-server-protocol.h"
#include <wayland-client.h>
#include <wayland-server.h>

#define VERSION 1

static void
on_toplevel_destroy(struct wl_listener *listener, void *data) {
    struct server_toplevel_decoration *toplevel_decoration =
        wl_container_of(listener, toplevel_decoration, on_destroy);

    wl_resource_post_error(
        toplevel_decoration->resource, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ORPHANED,
        "xdg_toplevel associated with zxdg_toplevel_decoration_v1 was destroyed");
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_xdg_decoration *xdg_decoration =
        wl_container_of(listener, xdg_decoration, display_destroy);

    wl_global_destroy(xdg_decoration->global);
    free(xdg_decoration);
}

static void
handle_xdg_toplevel_decoration_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_xdg_toplevel_decoration_set_mode(struct wl_client *client, struct wl_resource *resource,
                                        uint32_t mode) {
    if (mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE) {
        wl_client_post_implementation_error(
            client, "zxdg_toplevel_decoration_v1 must be set to server-side");
        return;
    }
}

static void
handle_xdg_toplevel_decoration_unset_mode(struct wl_client *client, struct wl_resource *resource) {
    // No relevant clients make use of this function.
    wl_client_post_implementation_error(
        client, "zxdg_toplevel_decoration_v1.unset_mode is not implemented");
}

static void
xdg_toplevel_decoration_destroy(struct wl_resource *resource) {
    struct server_toplevel_decoration *toplevel_decoration =
        server_toplevel_decoration_from_resource(resource);

    toplevel_decoration->parent->decoration = NULL;
    wl_list_remove(&toplevel_decoration->on_destroy.link);

    free(toplevel_decoration);
}

static const struct zxdg_toplevel_decoration_v1_interface xdg_toplevel_decoration_impl = {
    .destroy = handle_xdg_toplevel_decoration_destroy,
    .set_mode = handle_xdg_toplevel_decoration_set_mode,
    .unset_mode = handle_xdg_toplevel_decoration_unset_mode,
};

static void
handle_xdg_decoration_manager_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_xdg_decoration_manager_get_toplevel_decoration(struct wl_client *client,
                                                      struct wl_resource *resource, uint32_t id,
                                                      struct wl_resource *toplevel_resource) {
    struct server_xdg_toplevel *xdg_toplevel = server_xdg_toplevel_from_resource(toplevel_resource);
    struct server_surface *surface = xdg_toplevel->parent->parent;

    struct wl_resource *toplevel_decoration_resource = wl_resource_create(
        client, &zxdg_toplevel_decoration_v1_interface, wl_resource_get_version(resource), id);
    if (!toplevel_decoration_resource) {
        wl_client_post_no_memory(client);
        return;
    }

    if (surface->pending.buffer) {
        wl_resource_post_error(
            toplevel_decoration_resource, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_UNCONFIGURED_BUFFER,
            "xdg_toplevel given to zxdg_decoration_manager_v1.get_toplevel_decoration has a buffer "
            "attached");
        return;
    }
    if (surface->current_buffer) {
        wl_resource_post_error(
            toplevel_decoration_resource, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_UNCONFIGURED_BUFFER,
            "xdg_toplevel given to zxdg_decoration_manager_v1.get_toplevel_decoration has a buffer "
            "committed");
        return;
    }
    if (xdg_toplevel->decoration) {
        wl_resource_post_error(toplevel_decoration_resource,
                               ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED,
                               "xdg_toplevel given to zxdg_decoration_manager_v1.get_toplevel "
                               "already has an associated zxdg_toplevel_decoration_v1");
        return;
    }

    struct server_toplevel_decoration *toplevel_decoration =
        calloc(1, sizeof(*toplevel_decoration));
    if (!toplevel_decoration) {
        wl_client_post_no_memory(client);
        return;
    }

    toplevel_decoration->parent = xdg_toplevel;
    toplevel_decoration->resource = toplevel_decoration_resource;
    xdg_toplevel->decoration = toplevel_decoration;

    toplevel_decoration->on_destroy.notify = on_toplevel_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &toplevel_decoration->on_destroy);

    wl_resource_set_implementation(toplevel_decoration_resource, &xdg_toplevel_decoration_impl,
                                   toplevel_decoration, xdg_toplevel_decoration_destroy);

    zxdg_toplevel_decoration_v1_send_configure(toplevel_decoration_resource,
                                               ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    server_xdg_surface_send_configure(xdg_toplevel->parent);
}

static void
xdg_decoration_manager_destroy(struct wl_resource *resource) {
    // Unused.
}

static const struct zxdg_decoration_manager_v1_interface xdg_decoration_manager_impl = {
    .destroy = handle_xdg_decoration_manager_destroy,
    .get_toplevel_decoration = handle_xdg_decoration_manager_get_toplevel_decoration,
};

struct server_toplevel_decoration *
server_toplevel_decoration_from_resource(struct wl_resource *resource) {
    ww_assert(wl_resource_instance_of(resource, &zxdg_toplevel_decoration_v1_interface,
                                      &xdg_toplevel_decoration_impl));
    return wl_resource_get_user_data(resource);
}

struct server_xdg_decoration *
server_xdg_decoration_from_resource(struct wl_resource *resource) {
    ww_assert(wl_resource_instance_of(resource, &zxdg_decoration_manager_v1_interface,
                                      &xdg_decoration_manager_impl));
    return wl_resource_get_user_data(resource);
}

static void
handle_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= VERSION);
    struct server_xdg_decoration *xdg_decoration = data;

    struct wl_resource *resource =
        wl_resource_create(client, &zxdg_decoration_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }

    wl_resource_set_implementation(resource, &xdg_decoration_manager_impl, xdg_decoration,
                                   xdg_decoration_manager_destroy);
}

struct server_xdg_decoration *
server_xdg_decoration_create(struct server *server) {
    struct server_xdg_decoration *xdg_decoration = calloc(1, sizeof(*xdg_decoration));
    if (!xdg_decoration) {
        LOG(LOG_ERROR, "failed to allocate server_xdg_decoration");
        return NULL;
    }

    xdg_decoration->global =
        wl_global_create(server->display, &zxdg_decoration_manager_v1_interface, VERSION,
                         xdg_decoration, handle_bind);

    xdg_decoration->display_destroy.notify = on_display_destroy;

    wl_display_add_destroy_listener(server->display, &xdg_decoration->display_destroy);

    return xdg_decoration;
}
