#include "server/server.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "server/remote_buffer.h"
#include "server/ui.h"
#include "server/wl_compositor.h"
#include "server/wl_seat.h"
#include "server/wl_shm.h"
#include "server/wp_linux_dmabuf.h"
#include "server/xdg_decoration.h"
#include "server/xdg_shell.h"
#include "util.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <string.h>
#include <wayland-client.h>

#define USE_COMPOSITOR_VERSION 5
#define USE_LINUX_DMABUF_VERSION 4
#define USE_SEAT_VERSION 5
#define USE_SHM_VERSION 1
#define USE_SUBCOMPOSITOR_VERSION 1
#define USE_VIEWPORTER_VERSION 1
#define USE_XDG_WM_BASE_VERSION 1

static void
on_seat_capabilities(void *data, struct wl_seat *wl, uint32_t caps) {
    struct server_backend *backend = data;

    backend->seat_caps = caps;
    wl_signal_emit_mutable(&backend->events.seat_caps, &caps);
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
on_xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial) {
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = on_xdg_wm_base_ping,
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

        if (backend->seat) {
            ww_log(LOG_INFO, "received duplicate seat global");
            return;
        }

        backend->seat = wl_registry_bind(wl, name, &wl_seat_interface, USE_SEAT_VERSION);
        ww_assert(backend->seat);

        wl_seat_add_listener(backend->seat, &seat_listener, backend);
        wl_display_roundtrip(backend->display);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        if (version < USE_SHM_VERSION) {
            ww_log(LOG_ERROR, "host compositor provides outdated wl_shm (%d < %d)", version,
                   USE_SHM_VERSION);
            return;
        }

        backend->shm = wl_registry_bind(wl, name, &wl_shm_interface, USE_SHM_VERSION);
        ww_assert(backend->shm);

        wl_shm_add_listener(backend->shm, &shm_listener, backend);
        wl_display_roundtrip(backend->display);
    } else if (strcmp(iface, wl_subcompositor_interface.name) == 0) {
        if (version < USE_SUBCOMPOSITOR_VERSION) {
            ww_log(LOG_ERROR, "host compositor provides outdated wl_subcompositor (%d < %d)",
                   version, USE_SUBCOMPOSITOR_VERSION);
            return;
        }

        backend->subcompositor =
            wl_registry_bind(wl, name, &wl_subcompositor_interface, USE_SUBCOMPOSITOR_VERSION);
    } else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
        if (version < USE_VIEWPORTER_VERSION) {
            ww_log(LOG_ERROR, "host compositor provides outdated wp_viewporter (%d < %d)", version,
                   USE_VIEWPORTER_VERSION);
            return;
        }

        backend->viewporter =
            wl_registry_bind(wl, name, &wp_viewporter_interface, USE_VIEWPORTER_VERSION);
        ww_assert(backend->viewporter);
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        if (version < USE_XDG_WM_BASE_VERSION) {
            ww_log(LOG_ERROR, "host compositor provides outdated xdg_wm_base (%d < %d)", version,
                   USE_XDG_WM_BASE_VERSION);
            return;
        }

        backend->xdg_wm_base =
            wl_registry_bind(wl, name, &xdg_wm_base_interface, USE_XDG_WM_BASE_VERSION);
        ww_assert(backend->xdg_wm_base);

        xdg_wm_base_add_listener(backend->xdg_wm_base, &xdg_wm_base_listener, backend);
    }
}

static void
on_registry_global_remove(void *data, struct wl_registry *wl, uint32_t name) {
    // TODO: ???
}

static const struct wl_registry_listener registry_listener = {
    .global = on_registry_global,
    .global_remove = on_registry_global_remove,
};

static int
server_backend_create(struct server_backend *backend) {
    wl_array_init(&backend->shm_formats);

    wl_signal_init(&backend->events.seat_caps);
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
    if (!backend->subcompositor) {
        ww_log(LOG_ERROR, "host compositor does not provide wl_subcompositor");
        goto fail_registry;
    }
    if (!backend->viewporter) {
        ww_log(LOG_ERROR, "host compositor does not provide wp_viewporter");
        goto fail_registry;
    }
    if (!backend->xdg_wm_base) {
        ww_log(LOG_ERROR, "host compositor does not provide xdg_wm_base");
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

    wl_shm_destroy(backend->shm);
    wl_subcompositor_destroy(backend->subcompositor);
    wp_viewporter_destroy(backend->viewporter);
    xdg_wm_base_destroy(backend->xdg_wm_base);

    wl_registry_destroy(backend->registry);
    wl_display_disconnect(backend->display);
}

static int
server_backend_tick(int fd, uint32_t mask, void *data) {
    struct server *server = data;

    // Adapted from wlroots @ 31c842e5ece93145604c65be1b14c2f8cee24832
    // backend/wayland/backend.c:54

    if (mask & WL_EVENT_HANGUP) {
        ww_log(LOG_ERROR, "remote display hung up");
        wl_display_terminate(server->display);
        return 0;
    }

    if (mask & WL_EVENT_ERROR) {
        ww_log(LOG_ERROR, "failed to read events from remote display");
        wl_display_terminate(server->display);
        return 0;
    }

    if (mask & WL_EVENT_WRITABLE) {
        wl_display_flush(server->backend.display);
    }

    int dispatched = 0;
    if (mask & WL_EVENT_READABLE) {
        dispatched = wl_display_dispatch(server->backend.display);
    } else {
        dispatched = wl_display_dispatch_pending(server->backend.display);
        wl_display_flush(server->backend.display);
    }

    if (dispatched < 0) {
        ww_log(LOG_ERROR, "failed to dispatch events on remote display");
        wl_display_terminate(server->display);
        return 0;
    }

    return dispatched > 0;
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

    struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
    server->backend_source = wl_event_loop_add_fd(loop, wl_display_get_fd(server->backend.display),
                                                  WL_EVENT_READABLE, server_backend_tick, server);
    ww_assert(server->backend_source);
    wl_event_source_check(server->backend_source);

    server->remote_buf = remote_buffer_manager_create(server);
    if (!server->remote_buf) {
        goto fail_remote_buf;
    }

    server->compositor = server_compositor_g_create(server);
    if (!server->compositor) {
        goto fail_globals;
    }
    server->linux_dmabuf = server_linux_dmabuf_g_create(server);
    if (!server->linux_dmabuf) {
        goto fail_globals;
    }
    server->seat = server_seat_g_create(server);
    if (!server->seat) {
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

    if (server_ui_init(server, &server->ui) != 0) {
        ww_log(LOG_ERROR, "failed to initialize server_ui");
        goto fail_ui;
    }
    server_ui_show(&server->ui);

    return server;

fail_ui:
fail_globals:
fail_remote_buf:
    wl_event_source_remove(server->backend_source);
    wl_display_destroy(server->display);

fail_display:
    server_backend_destroy(&server->backend);

fail_backend:
    free(server);
    return NULL;
}

void
server_destroy(struct server *server) {
    wl_event_source_remove(server->backend_source);

    wl_display_destroy_clients(server->display);
    wl_display_destroy(server->display);

    server_ui_destroy(&server->ui);
    remote_buffer_manager_destroy(server->remote_buf);

    server_backend_destroy(&server->backend);
    free(server);
}

void
server_shutdown(struct server *server) {
    // TODO: make graceful (request window close, timeout)
    wl_display_terminate(server->display);
}
