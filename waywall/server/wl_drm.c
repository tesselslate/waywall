#include "server/wl_drm.h"
#include "server/backend.h"
#include "server/buffer.h"
#include "server/server.h"
#include "util/alloc.h"
#include "util/prelude.h"
#include "wayland-drm-client-protocol.h"
#include "wayland-drm-server-protocol.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <wayland-server-protocol.h>

#define SRV_DRM_VERSION 2

static void
on_drm_authenticated(void *data, struct wl_drm *drm) {
    struct server_drm_client *client = data;

    wl_drm_send_authenticated(client->resource);
}

static void
on_drm_capabilities(void *data, struct wl_drm *drm, uint32_t capabilities) {
    struct server_drm_client *client = data;

    wl_drm_send_capabilities(client->resource, capabilities);
}

static void
on_drm_device(void *data, struct wl_drm *drm, const char *name) {
    struct server_drm_client *client = data;

    wl_drm_send_device(client->resource, name);
}

static void
on_drm_format(void *data, struct wl_drm *drm, uint32_t format) {
    struct server_drm_client *client = data;

    wl_drm_send_format(client->resource, format);
}

static const struct wl_drm_listener drm_listener = {
    .authenticated = on_drm_authenticated,
    .capabilities = on_drm_capabilities,
    .device = on_drm_device,
    .format = on_drm_format,
};

static void
drm_resource_destroy(struct wl_resource *resource) {
    struct server_drm_client *drm_client = wl_resource_get_user_data(resource);

    wl_drm_destroy(drm_client->remote);
    free(drm_client);
}

static void
drm_authenticate(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    wl_client_post_implementation_error(client, "wl_drm.authenticate is not implemented");
}

static void
drm_create_buffer(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                  uint32_t name, int32_t width, int32_t height, uint32_t stride, uint32_t format) {
    wl_client_post_implementation_error(client, "wl_drm.create_buffer is not implemented");
}

static void
drm_create_planar_buffer(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                         uint32_t name, int32_t width, int32_t height, uint32_t format,
                         int32_t offset0, int32_t stride0, int32_t offset1, int32_t stride1,
                         int32_t offset2, int32_t stride2) {
    wl_client_post_implementation_error(client, "wl_drm.create_planar_buffer is not implemented");
}

static void
drm_create_prime_buffer(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                        int32_t name, int32_t width, int32_t height, uint32_t format,
                        int32_t offset0, int32_t stride0, int32_t offset1, int32_t stride1,
                        int32_t offset2, int32_t stride2) {
    // This is the only method from wl_drm which might be called by recent versions of Mesa and
    // Xwayland. It seems to be a fallback when linux_dmabuf is unavailable, though, so hopefully it
    // never gets used?
    wl_client_post_implementation_error(client, "wl_drm.create_prime_buffer is not implemented");
    close(name);
}

static const struct wl_drm_interface drm_impl = {
    .authenticate = drm_authenticate,
    .create_buffer = drm_create_buffer,
    .create_planar_buffer = drm_create_planar_buffer,
    .create_prime_buffer = drm_create_prime_buffer,
};

static void
on_global_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SRV_DRM_VERSION);

    struct server_drm *drm = data;

    struct server_drm_client *drm_client = zalloc(1, sizeof(*drm_client));

    drm_client->resource = wl_resource_create(client, &wl_drm_interface, version, id);
    check_alloc(drm_client->resource);
    wl_resource_set_implementation(drm_client->resource, &drm_impl, drm_client,
                                   drm_resource_destroy);

    drm_client->remote =
        wl_registry_bind(drm->server->backend->registry, drm->server->backend->drm.name,
                         &wl_drm_interface, SRV_DRM_VERSION);
    check_alloc(drm_client->remote);
    wl_drm_add_listener(drm_client->remote, &drm_listener, drm_client);

    wl_list_insert(&drm->objects, wl_resource_get_link(drm_client->resource));
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_drm *drm = wl_container_of(listener, drm, on_display_destroy);

    wl_global_destroy(drm->global);

    wl_list_remove(&drm->on_display_destroy.link);

    free(drm);
}

struct server_drm *
server_drm_create(struct server *server) {
    struct server_drm *drm = zalloc(1, sizeof(*drm));

    drm->global =
        wl_global_create(server->display, &wl_drm_interface, SRV_DRM_VERSION, drm, on_global_bind);
    check_alloc(drm->global);

    wl_list_init(&drm->objects);

    drm->server = server;

    drm->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &drm->on_display_destroy);

    return drm;
}
