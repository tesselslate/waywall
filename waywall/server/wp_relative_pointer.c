#include "server/wp_relative_pointer.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-server-protocol.h"
#include "server/server.h"
#include "server/wl_compositor.h"
#include "server/wl_seat.h"
#include "util.h"

#define SRV_RELATIVE_POINTER_VERSION 1

static void
on_relative_pointer_relative_motion(void *data, struct zwp_relative_pointer_v1 *relative_pointer,
                                    uint32_t utime_hi, uint32_t utime_lo, wl_fixed_t dx,
                                    wl_fixed_t dy, wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel) {
    struct server_relative_pointer_g *relative_pointer_g = data;

    if (!relative_pointer_g->seat_g->input_focus) {
        return;
    }

    struct wl_client *client =
        wl_resource_get_client(relative_pointer_g->seat_g->input_focus->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &relative_pointer_g->objects) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        // TODO: allow configuring mouse sensitivity
        zwp_relative_pointer_v1_send_relative_motion(resource, utime_hi, utime_lo, dx, dy,
                                                     dx_unaccel, dy_unaccel);
    }
}

static const struct zwp_relative_pointer_v1_listener relative_pointer_listener = {
    .relative_motion = on_relative_pointer_relative_motion,
};

static void
process_pointer(struct server_relative_pointer_g *relative_pointer_g, struct wl_pointer *pointer) {
    if (relative_pointer_g->remote_pointer) {
        zwp_relative_pointer_v1_destroy(relative_pointer_g->remote_pointer);
    }
    if (pointer) {
        relative_pointer_g->remote_pointer = zwp_relative_pointer_manager_v1_get_relative_pointer(
            relative_pointer_g->remote, pointer);
        if (!relative_pointer_g->remote_pointer) {
            ww_log(LOG_ERROR, "failed to get remote relative pointer");
            return;
        }

        zwp_relative_pointer_v1_add_listener(relative_pointer_g->remote_pointer,
                                             &relative_pointer_listener, relative_pointer_g);
    }
}

static void
relative_pointer_resource_destroy(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
}

static void
relative_pointer_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct zwp_relative_pointer_v1_interface relative_pointer_impl = {
    .destroy = relative_pointer_destroy,
};

static void
relative_pointer_manager_resource_destroy(struct wl_resource *resource) {
    // Unused.
}

static void
relative_pointer_manager_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
relative_pointer_manager_get_relative_pointer(struct wl_client *client,
                                              struct wl_resource *resource, uint32_t id,
                                              struct wl_resource *pointer_resource) {
    struct server_relative_pointer_g *relative_pointer_g = wl_resource_get_user_data(resource);

    struct wl_resource *relative_pointer_resource = wl_resource_create(
        client, &zwp_relative_pointer_v1_interface, wl_resource_get_version(resource), id);
    if (!relative_pointer_resource) {
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(relative_pointer_resource, &relative_pointer_impl,
                                   relative_pointer_g, relative_pointer_resource_destroy);

    wl_list_insert(&relative_pointer_g->objects, wl_resource_get_link(relative_pointer_resource));
}

static const struct zwp_relative_pointer_manager_v1_interface relative_pointer_manager_impl = {
    .destroy = relative_pointer_manager_destroy,
    .get_relative_pointer = relative_pointer_manager_get_relative_pointer,
};

static void
on_global_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SRV_RELATIVE_POINTER_VERSION);

    struct server_relative_pointer_g *relative_pointer_g = data;

    struct wl_resource *resource =
        wl_resource_create(client, &zwp_relative_pointer_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &relative_pointer_manager_impl, relative_pointer_g,
                                   relative_pointer_manager_resource_destroy);
}

static void
on_pointer(struct wl_listener *listener, void *data) {
    struct server_relative_pointer_g *relative_pointer_g =
        wl_container_of(listener, relative_pointer_g, on_pointer);

    struct wl_pointer *pointer = data;
    process_pointer(relative_pointer_g, pointer);
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_relative_pointer_g *relative_pointer_g =
        wl_container_of(listener, relative_pointer_g, on_display_destroy);

    wl_global_destroy(relative_pointer_g->global);

    if (relative_pointer_g->remote_pointer) {
        zwp_relative_pointer_v1_destroy(relative_pointer_g->remote_pointer);
    }

    wl_list_remove(&relative_pointer_g->on_pointer.link);
    wl_list_remove(&relative_pointer_g->on_display_destroy.link);

    free(relative_pointer_g);
}

struct server_relative_pointer_g *
server_relative_pointer_g_create(struct server *server) {
    struct server_relative_pointer_g *relative_pointer_g = calloc(1, sizeof(*relative_pointer_g));
    if (!relative_pointer_g) {
        ww_log(LOG_ERROR, "failed to allocate server_relative_pointer_g");
        return NULL;
    }

    relative_pointer_g->global =
        wl_global_create(server->display, &zwp_relative_pointer_manager_v1_interface,
                         SRV_RELATIVE_POINTER_VERSION, relative_pointer_g, on_global_bind);
    if (!relative_pointer_g->global) {
        ww_log(LOG_ERROR, "failed to allocate wl_relative_pointer global");
        free(relative_pointer_g);
        return NULL;
    }

    wl_list_init(&relative_pointer_g->objects);

    relative_pointer_g->seat_g = server->seat;

    relative_pointer_g->remote = server->backend.relative_pointer_manager;
    process_pointer(relative_pointer_g, server->seat->pointer);

    relative_pointer_g->on_pointer.notify = on_pointer;
    wl_signal_add(&server->seat->events.pointer, &relative_pointer_g->on_pointer);

    relative_pointer_g->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &relative_pointer_g->on_display_destroy);

    return relative_pointer_g;
}