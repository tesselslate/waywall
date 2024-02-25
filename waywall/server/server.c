#include "server/server.h"
#include "util.h"
#include <string.h>
#include <wayland-client.h>

static void
on_registry_global(void *data, struct wl_registry *wl, uint32_t name, const char *iface,
                   uint32_t version) {
    struct server_backend *backend = data;
}

static void
on_registry_global_remove(void *data, struct wl_registry *wl, uint32_t name) {
    struct server_backend *backend = data;
}

static const struct wl_registry_listener registry_listener = {
    .global = on_registry_global,
    .global_remove = on_registry_global_remove,
};

int
server_backend_create(struct server_backend *backend) {
    backend->display = wl_display_connect(NULL);
    if (!backend->display) {
        ww_log(LOG_ERROR, "wl_display_connect failed");
        goto fail_display;
    }
    backend->registry = wl_display_get_registry(backend->display);
    ww_assert(backend->registry);
    wl_registry_add_listener(backend->registry, &registry_listener, backend);
    wl_display_roundtrip(backend->display);

    return 0;

fail_display:
    wl_array_release(&backend->shm_formats);
    return 1;
}

static void
server_backend_destroy(struct server_backend *backend) {
    wl_registry_destroy(backend->registry);
    wl_display_disconnect(backend->display);
}

struct server *
server_create() {
    struct server *server = calloc(1, sizeof(*server));
    if (!server) {
        ww_log(LOG_ERROR, "failed to allocate server");
        return NULL;
    }

    if (server_backend_create(&server->backend) != 0) {
        goto fail_backend;
    }

    server->display = wl_display_create();
    ww_assert(server->display);

    return server;

fail_backend:
    free(server);
    return NULL;
}

void
server_destroy(struct server *server) {
    wl_display_destroy_clients(server->display);
    wl_display_destroy(server->display);

    server_backend_destroy(&server->backend);
    free(server);
}
