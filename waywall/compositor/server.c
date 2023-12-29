#include "compositor/server.h"
#include "compositor/wl_compositor.h"
#include "compositor/wl_output.h"
#include "compositor/wl_seat.h"
#include "compositor/wl_shm.h"
#include "compositor/wp_linux_dmabuf.h"
#include "compositor/wp_relative_pointer.h"
#include "compositor/xdg_decoration.h"
#include "compositor/xdg_shell.h"
#include "util.h"
#include <signal.h>
#include <stdbool.h>
#include <wayland-client.h>
#include <wayland-server.h>

#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

struct remote_seat {
    struct wl_list link; // server.remote.seats

    struct server *parent;
    struct wl_seat *remote;
    uint32_t name;

    uint32_t caps;
    char *title;

    bool active;
};

static void
remote_seat_destroy(struct remote_seat *remote_seat) {
    wl_seat_release(remote_seat->remote);

    if (remote_seat->title) {
        free(remote_seat->title);
    }

    wl_list_remove(&remote_seat->link);
    free(remote_seat);
}

static void
remote_seat_update(struct server *server) {
    struct remote_seat *remote_seat;

    wl_list_for_each (remote_seat, &server->remote.seats, link) {
        remote_seat->active = true;

        server_seat_set_remote(server->seat, remote_seat->remote);
        server_seat_set_caps(server->seat, remote_seat->caps);
        LOG(LOG_INFO, "updated active remote seat to '%s' (%" PRIu32 ")",
            remote_seat->title ? remote_seat->title : "unknown", remote_seat->name);
        return;
    }

    LOG(LOG_INFO, "no seats remaining to assign to server seat");
    server_seat_set_remote(server->seat, NULL);
}

static void
on_seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities) {
    struct remote_seat *remote_seat = data;

    if (remote_seat->active) {
        server_seat_set_caps(remote_seat->parent->seat, capabilities);
    }

    remote_seat->caps = capabilities;
}

static void
on_seat_name(void *data, struct wl_seat *wl_seat, const char *name) {
    struct remote_seat *remote_seat = data;

    if (remote_seat->title) {
        free(remote_seat->title);
    }

    remote_seat->title = strdup(name);
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = on_seat_capabilities,
    .name = on_seat_name,
};

static void
on_shm_format(void *data, struct wl_shm *shm, uint32_t format) {
    struct server *server = data;

    uint32_t *arr_format = wl_array_add(&server->remote.shm_formats, sizeof(uint32_t));
    *arr_format = format;
}

static const struct wl_shm_listener shm_listener = {
    .format = on_shm_format,
};

static void
on_registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
                   uint32_t version) {
    struct server *server = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        if (version < COMPOSITOR_REMOTE_VERSION) {
            LOG(LOG_ERROR, "found wl_compositor global of version %" PRIu32 " (wanted %d)", version,
                COMPOSITOR_REMOTE_VERSION);
            return;
        }

        server->remote.compositor =
            wl_registry_bind(registry, name, &wl_compositor_interface, COMPOSITOR_REMOTE_VERSION);
        ww_assert(server->remote.compositor);
        server->remote.compositor_id = name;
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        static_assert(SHM_REMOTE_VERSION == 1, "wl_shm remote version == 1");
        ww_assert(version >= SHM_REMOTE_VERSION);

        wl_array_init(&server->remote.shm_formats);

        server->remote.shm =
            wl_registry_bind(registry, name, &wl_shm_interface, SHM_REMOTE_VERSION);
        ww_assert(server->remote.shm);
        wl_shm_add_listener(server->remote.shm, &shm_listener, server);

        server->remote.shm_id = name;
        wl_display_roundtrip(server->remote.display);
    } else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
        ww_assert(version >= 1);

        server->remote.subcompositor =
            wl_registry_bind(registry, name, &wl_subcompositor_interface, 1);
        ww_assert(server->remote.subcompositor);
        server->remote.subcompositor_id = name;
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        ww_assert(version >= 1);

        server->remote.viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
        ww_assert(server->remote.viewporter);
        server->remote.viewporter_id = name;
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        ww_assert(version >= 5);

        server->remote.xdg_wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 5);
        ww_assert(server->remote.xdg_wm_base);
        server->remote.xdg_wm_base_id = name;
    } else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        if (version < LINUX_DMABUF_REMOTE_VERSION) {
            LOG(LOG_ERROR, "found zwp_linux_dmabuf_v1 global of version %" PRIu32 " (wanted %d)",
                version, LINUX_DMABUF_REMOTE_VERSION);
            return;
        }

        server->remote.linux_dmabuf = wl_registry_bind(
            registry, name, &zwp_linux_dmabuf_v1_interface, LINUX_DMABUF_REMOTE_VERSION);
        ww_assert(server->remote.linux_dmabuf);
        server->remote.linux_dmabuf_id = name;
    } else if (strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
        ww_assert(version >= 1);

        server->remote.relative_pointer_manager =
            wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1);
        ww_assert(server->remote.relative_pointer_manager);
        server->remote.relative_pointer_manager_id = name;
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        if (version < WL_SEAT_REMOTE_VERSION) {
            LOG(LOG_ERROR, "found wl_seat global of version %" PRIu32 " (wanted %d)", version,
                WL_SEAT_REMOTE_VERSION);
            return;
        }

        struct remote_seat *remote_seat = calloc(1, sizeof(*remote_seat));
        if (!remote_seat) {
            LOG(LOG_ERROR, "failed to allocate remote seat");
            return;
        }

        remote_seat->remote =
            wl_registry_bind(registry, name, &wl_seat_interface, WL_SEAT_REMOTE_VERSION);
        ww_assert(remote_seat->remote);
        wl_seat_add_listener(remote_seat->remote, &seat_listener, remote_seat);

        remote_seat->name = name;
        wl_list_insert(&server->remote.seats, &remote_seat->link);
        wl_display_roundtrip(server->remote.display);
    }
}

static void
on_registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    struct server *server = data;

    // TODO: meaningful error handling (crash?)

    if (name == server->remote.compositor_id) {
        LOG(LOG_ERROR, "host session compositor revoked wl_compositor global");
    } else if (name == server->remote.shm_id) {
        LOG(LOG_ERROR, "host session compositor revoked wl_shm global");
    } else if (name == server->remote.subcompositor_id) {
        LOG(LOG_ERROR, "host session compositor revoked wl_subcompositor global");
    } else if (name == server->remote.viewporter_id) {
        LOG(LOG_ERROR, "host session compositor revoked wp_viewporter global");
    } else if (name == server->remote.xdg_wm_base_id) {
        LOG(LOG_ERROR, "host session compositor revoked xdg_wm_base global");
    } else if (name == server->remote.linux_dmabuf_id) {
        LOG(LOG_ERROR, "host session compositor revoked zwp_linux_dmabuf_v1 global");
    } else if (name == server->remote.relative_pointer_manager_id) {
        LOG(LOG_ERROR, "host session compositor revoked zwp_relative_pointer_manager_v1 global");
    }

    struct remote_seat *remote_seat, *tmp;
    bool destroyed_active = false;

    wl_list_for_each_safe (remote_seat, tmp, &server->remote.seats, link) {
        if (remote_seat->name == name) {
            destroyed_active = remote_seat->active;
            remote_seat_destroy(remote_seat);
            break;
        }
    }

    if (destroyed_active) {
        remote_seat_update(server);
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = on_registry_global,
    .global_remove = on_registry_global_remove,
};

static int
process_remote_display(int fd, uint32_t mask, void *data) {
    struct server *server = data;

    // Adapted from wlroots' Wayland backend code.
    // backend/wayland/backend.c (dispatch_events)

    // Handle any errors.
    if (mask & WL_EVENT_HANGUP) {
        LOG(LOG_ERROR, "remote display hung up");
        wl_display_terminate(server->display);
        return 0;
    }
    if (mask & WL_EVENT_ERROR) {
        LOG(LOG_ERROR, "failed to read events from remote display");
        wl_display_terminate(server->display);
        return 0;
    }

    // Read and write events.
    if (mask & WL_EVENT_WRITABLE) {
        wl_display_flush(server->remote.display);
    }

    int ret = 0;
    if (mask & WL_EVENT_READABLE) {
        ret = wl_display_dispatch(server->remote.display);
    } else if (mask == 0) {
        ret = wl_display_dispatch_pending(server->remote.display);
        wl_display_flush(server->remote.display);
    }

    if (ret < 0) {
        LOG(LOG_ERROR, "failed to dispatch events on remote display");
        wl_display_terminate(server->display);
        return 0;
    }
    return ret > 0;
}

static int
process_sigint(int signal, void *data) {
    struct server *server = data;

    wl_display_terminate(server->display);
    return 0;
}

/*
 *  Public API
 */

struct server *
server_create() {
    struct server *server = calloc(1, sizeof(*server));
    if (!server) {
        LOG(LOG_ERROR, "failed to allocate server instance");
        return NULL;
    }

    server->display = wl_display_create();
    if (!server->display) {
        LOG(LOG_ERROR, "failed to create server display");
        goto fail_display_create;
    }

    server->remote.display = wl_display_connect(NULL);
    if (!server->remote.display) {
        LOG(LOG_ERROR, "failed to connect to remote display (no running compositor?)");
        goto fail_display_connect;
    }

    wl_list_init(&server->remote.seats);

    server->remote.registry = wl_display_get_registry(server->remote.display);
    ww_assert(server->remote.registry);
    wl_registry_add_listener(server->remote.registry, &registry_listener, server);
    wl_display_roundtrip(server->remote.display);

    if (!server->remote.compositor) {
        LOG(LOG_ERROR, "host session compositor did not provide wl_compositor global");
        goto fail_create_globals;
    }
    if (!server->remote.shm) {
        LOG(LOG_ERROR, "host session compositor did not provide wl_shm global");
        goto fail_create_globals;
    }
    if (!server->remote.subcompositor) {
        LOG(LOG_ERROR, "host session compositor did not provide wl_subcompositor global");
        goto fail_create_globals;
    }
    if (!server->remote.viewporter) {
        LOG(LOG_ERROR, "host session compositor did not provide wp_viewporter global");
        goto fail_create_globals;
    }
    if (!server->remote.xdg_wm_base) {
        LOG(LOG_ERROR, "host session compositor did not provide xdg_wm_base global");
        goto fail_create_globals;
    }
    if (!server->remote.linux_dmabuf) {
        LOG(LOG_ERROR, "host session compositor did not provide zwp_linux_dmabuf_v1 global");
        goto fail_create_globals;
    }
    if (!server->remote.relative_pointer_manager) {
        LOG(LOG_ERROR,
            "host session compostior did not provide zwp_relative_pointer_manager_v1 global");
        goto fail_create_globals;
    }

    server->seat = server_seat_create(server);
    if (!server->seat) {
        goto fail_create_globals;
    }

    server->compositor = server_compositor_create(server, server->remote.compositor);
    if (!server->compositor) {
        goto fail_create_globals;
    }

    if (!server_output_create(server)) {
        goto fail_create_globals;
    }
    if (!server_shm_create(server, server->remote.shm)) {
        goto fail_create_globals;
    }
    if (!server_xdg_wm_base_create(server, server->compositor)) {
        goto fail_create_globals;
    }
    if (!server_linux_dmabuf_create(server, server->remote.linux_dmabuf)) {
        goto fail_create_globals;
    }
    if (!server_relative_pointer_manager_create(server, server->seat,
                                                server->remote.relative_pointer_manager)) {
        goto fail_create_globals;
    }

    remote_seat_update(server);

    server->socket_name = wl_display_add_socket_auto(server->display);
    if (!server->socket_name) {
        LOG(LOG_ERROR, "failed to create server display socket");
        goto fail_add_socket;
    }

    struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
    server->source_remote = wl_event_loop_add_fd(loop, wl_display_get_fd(server->remote.display),
                                                 WL_EVENT_READABLE, process_remote_display, server);
    wl_event_source_check(server->source_remote);
    server->source_sigint = wl_event_loop_add_signal(loop, SIGINT, process_sigint, server);

    return server;

fail_add_socket:
fail_create_globals:
    wl_registry_destroy(server->remote.registry);
    wl_display_disconnect(server->remote.display);

fail_display_connect:
    wl_display_destroy(server->display);

fail_display_create:
    free(server);
    return NULL;
}

void
server_destroy(struct server *server) {
    ww_assert(server);

    wl_event_source_remove(server->source_remote);
    wl_event_source_remove(server->source_sigint);

    wl_display_destroy_clients(server->display);
    wl_display_destroy(server->display);

    struct remote_seat *remote_seat, *tmp;
    wl_list_for_each_safe (remote_seat, tmp, &server->remote.seats, link) {
        remote_seat_destroy(remote_seat);
    }
    wl_shm_destroy(server->remote.shm);
    wl_array_release(&server->remote.shm_formats);

    wl_registry_destroy(server->remote.registry);
    wl_display_disconnect(server->remote.display);

    free(server);
}

bool
server_run(struct server *server) {
    ww_assert(server);

    wl_display_run(server->display);

    // TODO: error reporting
    return true;
}

int
main() {
    struct server *s = server_create();
    if (!s) {
        return 1;
    }
    server_run(s);
    server_destroy(s);
    printf("ok\n");
}
