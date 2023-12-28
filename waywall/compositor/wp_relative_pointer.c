#include "compositor/wp_relative_pointer.h"
#include "compositor/server.h"
#include "compositor/wl_compositor.h"
#include "compositor/wl_seat.h"
#include "util.h"
#include <wayland-client.h>
#include <wayland-server.h>

#include "relative-pointer-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-server-protocol.h"

#define VERSION 1

struct server_relative_pointer {
    struct wl_resource *resource;
    struct zwp_relative_pointer_v1 *remote;

    struct server_seat *seat;
    struct wl_listener seat_destroy;
};

static void
pointer_on_seat_destroy(struct wl_listener *listener, void *data) {
    struct server_relative_pointer *relative_pointer =
        wl_container_of(listener, relative_pointer, seat_destroy);

    wl_resource_destroy(relative_pointer->resource);
}

static void
manager_on_seat_destroy(struct wl_listener *listener, void *data) {
    struct server_relative_pointer_manager *manager =
        wl_container_of(listener, manager, seat_destroy);

    manager->seat = NULL;
}

static void
on_relative_pointer_relative_motion(void *data, struct zwp_relative_pointer_v1 *wp_relative_pointer,
                                    uint32_t utime_hi, uint32_t utime_lo, wl_fixed_t dx,
                                    wl_fixed_t dy, wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel) {
    struct server_relative_pointer *relative_pointer = data;

    if (!relative_pointer->seat->input_focus) {
        return;
    }

    struct wl_client *input_focus_client = wl_resource_get_client(
        server_view_get_surface(relative_pointer->seat->input_focus)->resource);

    if (input_focus_client == wl_resource_get_client(relative_pointer->resource)) {
        // TODO: allow users to change mouse sens
        zwp_relative_pointer_v1_send_relative_motion(relative_pointer->resource, utime_hi, utime_lo,
                                                     dx, dy, dx_unaccel, dy_unaccel);
    }
}

static const struct zwp_relative_pointer_v1_listener relative_pointer_listener = {
    .relative_motion = on_relative_pointer_relative_motion,
};

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_relative_pointer_manager *manager =
        wl_container_of(listener, manager, display_destroy);

    zwp_relative_pointer_manager_v1_destroy(manager->remote);
    wl_global_destroy(manager->global);

    free(manager);
}

static void
handle_relative_pointer_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
relative_pointer_destroy(struct wl_resource *resource) {
    struct server_relative_pointer *relative_pointer = wl_resource_get_user_data(resource);

    zwp_relative_pointer_v1_destroy(relative_pointer->remote);
    free(relative_pointer);
}

static const struct zwp_relative_pointer_v1_interface relative_pointer_impl = {
    .destroy = handle_relative_pointer_destroy,
};

static void
handle_relative_pointer_manager_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_relative_pointer_manager_get_relative_pointer(struct wl_client *client,
                                                     struct wl_resource *resource, uint32_t id,
                                                     struct wl_resource *pointer_resource) {
    struct server_relative_pointer_manager *manager =
        server_relative_pointer_manager_from_resource(resource);

    struct wl_resource *relative_pointer_resource = wl_resource_create(
        client, &zwp_relative_pointer_v1_interface, wl_resource_get_version(resource), id);
    if (!relative_pointer_resource) {
        wl_client_post_no_memory(client);
        return;
    }

    if (!manager->seat) {
        wl_client_post_implementation_error(client, "no seat to give a relative pointer from");
        return;
    }

    struct server_relative_pointer *relative_pointer = calloc(1, sizeof(*relative_pointer));
    if (!relative_pointer) {
        wl_client_post_no_memory(client);
        return;
    }

    relative_pointer->resource = relative_pointer_resource;
    relative_pointer->seat = manager->seat;

    relative_pointer->remote = zwp_relative_pointer_manager_v1_get_relative_pointer(
        manager->remote, manager->seat->remote.pointer);
    zwp_relative_pointer_v1_add_listener(relative_pointer->remote, &relative_pointer_listener,
                                         relative_pointer);

    wl_resource_set_implementation(relative_pointer_resource, &relative_pointer_impl,
                                   relative_pointer, relative_pointer_destroy);

    relative_pointer->seat_destroy.notify = pointer_on_seat_destroy;
    wl_signal_add(&manager->seat->events.destroy, &relative_pointer->seat_destroy);
}

static void
relative_pointer_manager_destroy(struct wl_resource *resource) {
    // Unused.
}

static const struct zwp_relative_pointer_manager_v1_interface relative_pointer_manager_impl = {
    .destroy = handle_relative_pointer_manager_destroy,
    .get_relative_pointer = handle_relative_pointer_manager_get_relative_pointer,
};

struct server_relative_pointer_manager *
server_relative_pointer_manager_from_resource(struct wl_resource *resource) {
    ww_assert(wl_resource_instance_of(resource, &zwp_relative_pointer_manager_v1_interface,
                                      &relative_pointer_manager_impl));
    return wl_resource_get_user_data(resource);
}

static void
handle_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= VERSION);
    struct server_relative_pointer_manager *manager = data;

    struct wl_resource *resource =
        wl_resource_create(client, &zwp_relative_pointer_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }

    wl_resource_set_implementation(resource, &relative_pointer_manager_impl, manager,
                                   relative_pointer_manager_destroy);
}

struct server_relative_pointer_manager *
server_relative_pointer_manager_create(struct server *server, struct server_seat *seat,
                                       struct zwp_relative_pointer_manager_v1 *remote) {
    struct server_relative_pointer_manager *manager = calloc(1, sizeof(*manager));
    if (!manager) {
        LOG(LOG_ERROR, "failed to allocate server_relative_pointer_manager");
        return NULL;
    }

    manager->remote = remote;
    manager->seat = seat;

    manager->global = wl_global_create(server->display, &zwp_relative_pointer_manager_v1_interface,
                                       VERSION, manager, handle_bind);

    manager->display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &manager->display_destroy);

    manager->seat_destroy.notify = manager_on_seat_destroy;
    wl_signal_add(&seat->events.destroy, &manager->seat_destroy);

    return manager;
}
