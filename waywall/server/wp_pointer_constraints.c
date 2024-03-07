#include "server/wp_pointer_constraints.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-server-protocol.h"
#include "server/server.h"
#include "server/wl_seat.h"
#include "util.h"
#include <stdlib.h>

#define SRV_POINTER_CONSTRAINTS_VERSION 1

static void
process_pointer(struct server_pointer_constraints_g *pointer_constraints_g,
                struct wl_pointer *pointer) {
    // TODO: recreate remote locked pointer
}

static void
locked_pointer_resource_destroy(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
}

static void
locked_pointer_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
locked_pointer_set_cursor_position_hint(struct wl_client *client, struct wl_resource *resource,
                                        wl_fixed_t surface_x, wl_fixed_t surface_y) {
    // Unused. We don't care what the game wants to set the cursor position to.
}

static void
locked_pointer_set_region(struct wl_client *client, struct wl_resource *resource,
                          struct wl_resource *region_resource) {
    // Unused. We don't care about what region the game wants to lock the pointer to.
}

static const struct zwp_locked_pointer_v1_interface locked_pointer_impl = {
    .destroy = locked_pointer_destroy,
    .set_cursor_position_hint = locked_pointer_set_cursor_position_hint,
    .set_region = locked_pointer_set_region,
};

static void
pointer_constraints_resource_destroy(struct wl_resource *resource) {
    // Unused.
}

static void
pointer_constraints_confine_pointer(struct wl_client *client, struct wl_resource *resource,
                                    uint32_t id, struct wl_resource *surface_resource,
                                    struct wl_resource *pointer_resource,
                                    struct wl_resource *region_resource, uint32_t lifetime) {
    // Unused.
    wl_client_post_implementation_error(client,
                                        "zwp_pointer_constraints.confine_pointer is not supported");
}

static void
pointer_constraints_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
pointer_constraints_lock_pointer(struct wl_client *client, struct wl_resource *resource,
                                 uint32_t id, struct wl_resource *surface_resource,
                                 struct wl_resource *pointer_resource,
                                 struct wl_resource *region_resource, uint32_t lifetime) {
    struct server_pointer_constraints_g *pointer_constraints_g =
        wl_resource_get_user_data(resource);

    struct wl_resource *locked_pointer_resource = wl_resource_create(
        client, &zwp_locked_pointer_v1_interface, wl_resource_get_version(resource), id);
    if (!locked_pointer_resource) {
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(locked_pointer_resource, &locked_pointer_impl,
                                   pointer_constraints_g, locked_pointer_resource_destroy);

    // TODO: create remote locked pointer

    wl_list_insert(&pointer_constraints_g->objects, wl_resource_get_link(locked_pointer_resource));
}

static const struct zwp_pointer_constraints_v1_interface pointer_constraints_impl = {
    .confine_pointer = pointer_constraints_confine_pointer,
    .destroy = pointer_constraints_destroy,
    .lock_pointer = pointer_constraints_lock_pointer,
};

static void
on_global_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SRV_POINTER_CONSTRAINTS_VERSION);

    struct server_pointer_constraints_g *pointer_constraints_g = data;

    struct wl_resource *resource =
        wl_resource_create(client, &zwp_pointer_constraints_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &pointer_constraints_impl, pointer_constraints_g,
                                   pointer_constraints_resource_destroy);
}

static void
on_pointer(struct wl_listener *listener, void *data) {
    struct server_pointer_constraints_g *pointer_constraints_g =
        wl_container_of(listener, pointer_constraints_g, on_pointer);

    struct wl_pointer *pointer = data;
    process_pointer(pointer_constraints_g, pointer);
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_pointer_constraints_g *pointer_constraints_g =
        wl_container_of(listener, pointer_constraints_g, on_display_destroy);

    wl_global_destroy(pointer_constraints_g->global);

    if (pointer_constraints_g->remote_pointer) {
        zwp_locked_pointer_v1_destroy(pointer_constraints_g->remote_pointer);
    }

    wl_list_remove(&pointer_constraints_g->on_pointer.link);
    wl_list_remove(&pointer_constraints_g->on_display_destroy.link);

    free(pointer_constraints_g);
}

struct server_pointer_constraints_g *
server_pointer_constraints_g_create(struct server *server) {
    struct server_pointer_constraints_g *pointer_constraints_g =
        calloc(1, sizeof(*pointer_constraints_g));
    if (!pointer_constraints_g) {
        ww_log(LOG_ERROR, "failed to allocate server_pointer_constraints_g");
        return NULL;
    }

    pointer_constraints_g->global =
        wl_global_create(server->display, &zwp_pointer_constraints_v1_interface,
                         SRV_POINTER_CONSTRAINTS_VERSION, pointer_constraints_g, on_global_bind);
    if (!pointer_constraints_g->global) {
        ww_log(LOG_ERROR, "failed to allocate wl_pointer_constraints global");
        free(pointer_constraints_g);
        return NULL;
    }

    wl_list_init(&pointer_constraints_g->objects);

    pointer_constraints_g->seat_g = server->seat;

    pointer_constraints_g->remote = server->backend.pointer_constraints;
    process_pointer(pointer_constraints_g, server->seat->pointer);

    pointer_constraints_g->on_pointer.notify = on_pointer;
    wl_signal_add(&server->seat->events.pointer, &pointer_constraints_g->on_pointer);

    pointer_constraints_g->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &pointer_constraints_g->on_display_destroy);

    return pointer_constraints_g;
}
