#include "server/server.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "server/wl_compositor.h"
#include "server/wl_shm.h"
#include "server/wp_linux_dmabuf.h"
#include "server/xdg_decoration.h"
#include "server/xdg_shell.h"
#include "util.h"
#include <string.h>
#include <wayland-client.h>

#define USE_COMPOSITOR_VERSION 5
#define USE_LINUX_DMABUF_VERSION 4
#define USE_SEAT_VERSION 5
#define USE_SHM_VERSION 1

struct backend_seat {
    struct wl_list link; // server_backend.seats

    struct wl_seat *wl;
    uint32_t name;

    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
};

static void
seat_destroy(struct backend_seat *seat) {
    wl_seat_release(seat->wl);

    wl_list_remove(&seat->link);
    free(seat);
}

static void
on_seat_capabilities(void *data, struct wl_seat *wl, uint32_t capabilities) {
    struct backend_seat *seat = data;

    bool has_ptr = !!seat->pointer;
    bool cap_ptr = capabilities & WL_SEAT_CAPABILITY_POINTER;
    if (!has_ptr && cap_ptr) {
        seat->pointer = wl_seat_get_pointer(seat->wl);
        ww_assert(seat->pointer);
    } else if (has_ptr && !cap_ptr) {
        wl_pointer_release(seat->pointer);
        seat->pointer = NULL;
    }

    bool has_kb = !!seat->keyboard;
    bool cap_kb = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
    if (!has_kb && cap_kb) {
        seat->keyboard = wl_seat_get_keyboard(seat->wl);
        ww_assert(seat->keyboard);
    } else if (has_kb && !cap_kb) {
        wl_keyboard_release(seat->keyboard);
        seat->keyboard = NULL;
    }
}

static void
on_seat_name(void *data, struct wl_seat *wl, const char *name) {
    // Unused.
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = on_seat_capabilities,
    .name = on_seat_name,
};

static void
on_shm_format(void *data, struct wl_shm *wl, uint32_t format) {
    struct server_backend *backend = data;

    // I think an assert for allocation failure here is alright because we can't do much about it
    // and this event should only ever be received at startup.
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
    } else if (strcmp(iface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        if (version < USE_LINUX_DMABUF_VERSION) {
            ww_log(LOG_ERROR, "host compositor provides outdated zwp_linux_dmabuf (%d < %d)",
                   version, USE_LINUX_DMABUF_VERSION);
            return;
        }

        backend->linux_dmabuf =
            wl_registry_bind(wl, name, &zwp_linux_dmabuf_v1_interface, USE_LINUX_DMABUF_VERSION);
        ww_assert(backend->linux_dmabuf);
    } else if (strcmp(iface, wl_seat_interface.name) == 0) {
        if (version < USE_SEAT_VERSION) {
            ww_log(LOG_ERROR, "host compositor provides outdated wl_seat (%d < %d)", version,
                   USE_SEAT_VERSION);
            return;
        }

        struct backend_seat *seat = calloc(1, sizeof(*seat));
        if (!seat) {
            ww_log(LOG_ERROR, "failed to allocate backend_seat");
            return;
        }
        seat->wl = wl_registry_bind(wl, name, &wl_seat_interface, USE_SEAT_VERSION);
        ww_assert(seat->wl);
        seat->name = name;

        wl_seat_add_listener(seat->wl, &seat_listener, seat);
        wl_list_insert(&backend->seats, &seat->link);
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

    struct backend_seat *seat, *tmp_seat;
    wl_list_for_each_safe (seat, tmp_seat, &backend->seats, link) {
        if (seat->name == name) {
            seat_destroy(seat);
            return;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = on_registry_global,
    .global_remove = on_registry_global_remove,
};

int
server_backend_create(struct server_backend *backend) {
    wl_list_init(&backend->seats);
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
    if (!backend->linux_dmabuf) {
        ww_log(LOG_ERROR, "host compositor does not provide zwp_linux_dmabuf");
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

    struct backend_seat *seat, *seat_tmp;
    wl_list_for_each_safe (seat, seat_tmp, &backend->seats, link) {
        seat_destroy(seat);
    }

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
    if (!server->display) {
        goto fail_display;
    }

    server->compositor = server_compositor_g_create(server);
    if (!server->compositor) {
        goto fail_globals;
    }
    server->linux_dmabuf = server_linux_dmabuf_g_create(server);
    if (!server->linux_dmabuf) {
        goto fail_globals;
    }
    server->shm = server_shm_g_create(server);
    if (!server->shm) {
        goto fail_globals;
    }
    server->xdg_shell = server_xdg_wm_base_g_create(server);
    if (!server->xdg_shell) {
        goto fail_globals;
    }
    server->xdg_decoration = server_xdg_decoration_manager_g_create(server);
    if (!server->xdg_decoration) {
        goto fail_globals;
    }

    return server;

fail_globals:
    wl_display_destroy(server->display);

fail_display:
    server_backend_destroy(&server->backend);

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

void
server_shutdown(struct server *server) {
    // TODO: make graceful (request window close, timeout)
    wl_display_terminate(server->display);
}
