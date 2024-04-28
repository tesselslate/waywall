#include "server/xdg_decoration.h"
#include "server/server.h"
#include "server/wl_compositor.h"
#include "server/xdg_shell.h"
#include "util/alloc.h"
#include "util/prelude.h"
#include "xdg-decoration-unstable-v1-server-protocol.h"
#include <stdint.h>
#include <stdlib.h>

#define SRV_XDG_DECORATION_MANAGER_VERSION 1

static void
on_toplevel_destroy(struct wl_listener *listener, void *data) {
    struct server_xdg_toplevel_decoration *toplevel_decoration =
        wl_container_of(listener, toplevel_decoration, on_toplevel_destroy);

    wl_resource_post_error(toplevel_decoration->resource,
                           ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ORPHANED,
                           "xdg_toplevel destroyed before associated zxdg_toplevel_decoration");
}

static void
xdg_toplevel_decoration_resource_destroy(struct wl_resource *resource) {
    struct server_xdg_toplevel_decoration *toplevel_decoration =
        wl_resource_get_user_data(resource);

    wl_list_remove(&toplevel_decoration->link);
    wl_list_remove(&toplevel_decoration->on_toplevel_destroy.link);
    free(toplevel_decoration);
}

static void
xdg_toplevel_decoration_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
xdg_toplevel_decoration_set_mode(struct wl_client *client, struct wl_resource *resource,
                                 uint32_t mode) {
    struct server_xdg_toplevel_decoration *toplevel_decoration =
        wl_resource_get_user_data(resource);

    zxdg_toplevel_decoration_v1_send_configure(toplevel_decoration->resource,
                                               ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    server_xdg_surface_send_configure(toplevel_decoration->xdg_toplevel->parent);
}

static void
xdg_toplevel_decoration_unset_mode(struct wl_client *client, struct wl_resource *resource) {
    struct server_xdg_toplevel_decoration *toplevel_decoration =
        wl_resource_get_user_data(resource);

    zxdg_toplevel_decoration_v1_send_configure(toplevel_decoration->resource,
                                               ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    server_xdg_surface_send_configure(toplevel_decoration->xdg_toplevel->parent);
}

static const struct zxdg_toplevel_decoration_v1_interface xdg_toplevel_decoration_impl = {
    .destroy = xdg_toplevel_decoration_destroy,
    .set_mode = xdg_toplevel_decoration_set_mode,
    .unset_mode = xdg_toplevel_decoration_unset_mode,
};

static void
xdg_decoration_manager_resource_destroy(struct wl_resource *resource) {
    // Unused.
}

static void
xdg_decoration_manager_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
xdg_decoration_manager_get_toplevel_decoration(struct wl_client *client,
                                               struct wl_resource *resource, uint32_t id,
                                               struct wl_resource *toplevel_resource) {
    struct server_xdg_decoration_manager *xdg_decoration_manager =
        wl_resource_get_user_data(resource);
    struct server_xdg_toplevel *xdg_toplevel = server_xdg_toplevel_from_resource(toplevel_resource);

    struct server_xdg_toplevel_decoration *toplevel_decoration =
        zalloc(1, sizeof(*toplevel_decoration));

    toplevel_decoration->xdg_toplevel = xdg_toplevel;

    toplevel_decoration->resource = wl_resource_create(
        client, &zxdg_toplevel_decoration_v1_interface, wl_resource_get_version(resource), id);
    check_alloc(toplevel_decoration->resource);
    wl_resource_set_implementation(toplevel_decoration->resource, &xdg_toplevel_decoration_impl,
                                   toplevel_decoration, xdg_toplevel_decoration_resource_destroy);

    struct server_xdg_toplevel_decoration *elem;
    wl_list_for_each (elem, &xdg_decoration_manager->decorations, link) {
        if (elem->xdg_toplevel == xdg_toplevel) {
            wl_resource_post_error(
                toplevel_decoration->resource,
                ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED,
                "xdg_toplevel already has an associated zxdg_toplevel_decoration");
            return;
        }
    }

    struct server_surface *surface = xdg_toplevel->parent->parent;
    if (surface->current.buffer || surface->pending.buffer) {
        wl_resource_post_error(toplevel_decoration->resource,
                               ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED,
                               "xdg_toplevel associated with new zxdg_toplevel_decoration already "
                               "has an attached buffer");
        return;
    }

    toplevel_decoration->parent = xdg_decoration_manager;
    toplevel_decoration->on_toplevel_destroy.notify = on_toplevel_destroy;
    wl_list_insert(&xdg_decoration_manager->decorations, &toplevel_decoration->link);
    wl_signal_add(&xdg_toplevel->events.destroy, &toplevel_decoration->on_toplevel_destroy);

    zxdg_toplevel_decoration_v1_send_configure(toplevel_decoration->resource,
                                               ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    server_xdg_surface_send_configure(toplevel_decoration->xdg_toplevel->parent);
}

static const struct zxdg_decoration_manager_v1_interface xdg_decoration_manager_impl = {
    .destroy = xdg_decoration_manager_destroy,
    .get_toplevel_decoration = xdg_decoration_manager_get_toplevel_decoration,
};

static void
on_global_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SRV_XDG_DECORATION_MANAGER_VERSION);

    struct server_xdg_decoration_manager *xdg_decoration_manager = data;

    struct wl_resource *resource =
        wl_resource_create(client, &zxdg_decoration_manager_v1_interface, version, id);
    check_alloc(resource);
    wl_resource_set_implementation(resource, &xdg_decoration_manager_impl, xdg_decoration_manager,
                                   xdg_decoration_manager_resource_destroy);
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_xdg_decoration_manager *xdg_decoration_manager =
        wl_container_of(listener, xdg_decoration_manager, on_display_destroy);

    wl_global_destroy(xdg_decoration_manager->global);

    wl_list_remove(&xdg_decoration_manager->on_display_destroy.link);

    free(xdg_decoration_manager);
}

struct server_xdg_decoration_manager *
server_xdg_decoration_manager_create(struct server *server) {
    struct server_xdg_decoration_manager *xdg_decoration_manager =
        zalloc(1, sizeof(*xdg_decoration_manager));

    xdg_decoration_manager->global = wl_global_create(
        server->display, &zxdg_decoration_manager_v1_interface, SRV_XDG_DECORATION_MANAGER_VERSION,
        xdg_decoration_manager, on_global_bind);
    check_alloc(xdg_decoration_manager->global);

    xdg_decoration_manager->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &xdg_decoration_manager->on_display_destroy);

    wl_list_init(&xdg_decoration_manager->decorations);

    return xdg_decoration_manager;
}
