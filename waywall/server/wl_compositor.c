#include "server/wl_compositor.h"
#include "server/backend.h"
#include "server/buffer.h"
#include "server/server.h"
#include "server/surface.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include <inttypes.h>
#include <stdlib.h>
#include <wayland-client-protocol.h>
#include <wayland-server-protocol.h>

static constexpr int SRV_COMPOSITOR_VERSION = 5;

static void
region_resource_destroy(struct wl_resource *resource) {
    struct server_region *region = wl_resource_get_user_data(resource);

    wl_region_destroy(region->remote);
    free(region);
}

static void
region_add(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y,
           int32_t width, int32_t height) {
    struct server_region *region = wl_resource_get_user_data(resource);

    wl_region_add(region->remote, x, y, width, height);
}

static void
region_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
region_subtract(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y,
                int32_t width, int32_t height) {
    struct server_region *region = wl_resource_get_user_data(resource);

    wl_region_subtract(region->remote, x, y, width, height);
}

static const struct wl_region_interface region_impl = {
    .add = region_add,
    .destroy = region_destroy,
    .subtract = region_subtract,
};

static void
compositor_resource_destroy(struct wl_resource *resource) {
    // Unused.
}

static void
compositor_create_region(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_compositor *compositor = wl_resource_get_user_data(resource);

    struct server_region *region = zalloc(1, sizeof(*region));

    region->resource =
        wl_resource_create(client, &wl_region_interface, wl_resource_get_version(resource), id);
    check_alloc(region->resource);
    wl_resource_set_implementation(region->resource, &region_impl, region, region_resource_destroy);

    region->remote = wl_compositor_create_region(compositor->remote);
    check_alloc(region->remote);
}

static void
compositor_create_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_compositor *compositor = wl_resource_get_user_data(resource);

    struct wl_resource *surface_resource =
        wl_resource_create(client, &wl_surface_interface, wl_resource_get_version(resource), id);
    check_alloc(surface_resource);
    struct server_surface *surface = server_surface_create(surface_resource);

    surface->remote = wl_compositor_create_surface(compositor->remote);
    check_alloc(surface->remote);

    // We need to ensure that input events are never given to a child surface. See
    // `surface_set_input_region` for more details.
    wl_surface_set_input_region(surface->remote, compositor->empty_region);

    surface->parent = compositor;

    wl_signal_init(&surface->events.commit);
    wl_signal_init(&surface->events.destroy);

    wl_signal_emit_mutable(&compositor->events.new_surface, surface);
}

static const struct wl_compositor_interface compositor_impl = {
    .create_region = compositor_create_region,
    .create_surface = compositor_create_surface,
};

static void
on_global_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SRV_COMPOSITOR_VERSION);

    struct server_compositor *compositor = data;

    struct wl_resource *resource =
        wl_resource_create(client, &wl_compositor_interface, version, id);
    check_alloc(resource);
    wl_resource_set_implementation(resource, &compositor_impl, compositor,
                                   compositor_resource_destroy);
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_compositor *compositor =
        wl_container_of(listener, compositor, on_display_destroy);

    wl_signal_emit_mutable(&compositor->events.destroy, compositor);

    wl_region_destroy(compositor->empty_region);
    wl_global_destroy(compositor->global);

    wl_list_remove(&compositor->on_display_destroy.link);

    free(compositor);
}

struct server_compositor *
server_compositor_create(struct server *server) {
    struct server_compositor *compositor = zalloc(1, sizeof(*compositor));

    compositor->global = wl_global_create(server->display, &wl_compositor_interface,
                                          SRV_COMPOSITOR_VERSION, compositor, on_global_bind);
    check_alloc(compositor->global);

    compositor->remote = server->backend->compositor;
    compositor->empty_region = wl_compositor_create_region(compositor->remote);

    compositor->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &compositor->on_display_destroy);

    wl_signal_init(&compositor->events.destroy);
    wl_signal_init(&compositor->events.new_surface);

    return compositor;
}

struct server_region *
server_region_from_resource(struct wl_resource *resource) {
    ww_assert(wl_resource_instance_of(resource, &wl_region_interface, &region_impl));
    return wl_resource_get_user_data(resource);
}
