#include "server/server.h"
#include "util.h"
#include <string.h>
#include <wayland-client.h>

#define USE_COMPOSITOR_VERSION 5
#define USE_SHM_VERSION 1

static void
on_shm_format(void *data, struct wl_shm *wl, uint32_t format) {
    struct server_backend *backend = data;

    uint32_t *next = wl_array_add(&backend->shm_formats, sizeof(*next));
    ww_assert(next);
    *next = format;

    wl_signal_emit_mutable(&backend->events.shm_format, next);
}

static const struct wl_shm_listener shm_listener = {
    .format = on_shm_format,
};

static void
on_registry_global(void *data, struct wl_registry *wl, uint32_t name, const char *iface,
                   uint32_t version) {
    struct server_backend *backend = data;

    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        if (version < USE_COMPOSITOR_VERSION) {
            ww_log(LOG_ERROR, "host compositor provides outdated wl_compositor (%d < %d)", version,
                   USE_COMPOSITOR_VERSION);
            return;
        }

        backend->compositor =
            wl_registry_bind(wl, name, &wl_compositor_interface, USE_COMPOSITOR_VERSION);
        ww_assert(backend->compositor);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        if (version < USE_SHM_VERSION) {
            ww_log(LOG_ERROR, "host compositor provides outdated wl_shm (%d < %d)", version,
                   USE_SHM_VERSION);
            return;
        }

        backend->shm = wl_registry_bind(wl, name, &wl_shm_interface, USE_SHM_VERSION);
        ww_assert(backend->shm);

        wl_shm_add_listener(backend->shm, &shm_listener, backend);
    }
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
    wl_array_init(&backend->shm_formats);

    wl_signal_init(&backend->events.shm_format);

    backend->display = wl_display_connect(NULL);
    if (!backend->display) {
        ww_log(LOG_ERROR, "wl_display_connect failed");
        goto fail_display;
    }
    backend->registry = wl_display_get_registry(backend->display);
    ww_assert(backend->registry);
    wl_registry_add_listener(backend->registry, &registry_listener, backend);
    wl_display_roundtrip(backend->display);

    if (!backend->compositor) {
        ww_log(LOG_ERROR, "host compositor does not provide wl_compositor");
        goto fail_registry;
    }
    if (!backend->shm) {
        ww_log(LOG_ERROR, "host compositor does not provide wl_shm");
        goto fail_registry;
    }

    return 0;

fail_registry:
    wl_registry_destroy(backend->registry);
    wl_display_disconnect(backend->display);

fail_display:
    wl_array_release(&backend->shm_formats);
    return 1;
}

static void
server_backend_destroy(struct server_backend *backend) {
    wl_array_release(&backend->shm_formats);

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
