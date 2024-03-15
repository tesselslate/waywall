#include "server/wp_pointer_constraints.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-server-protocol.h"
#include "server/server.h"
#include "server/wl_compositor.h"
#include "server/wl_seat.h"
#include "util.h"
#include <stdlib.h>

#define SRV_POINTER_CONSTRAINTS_VERSION 1

static void
locked_pointer_resource_destroy(struct wl_resource *resource) {
    struct server_pointer_constraints *pointer_constraints = wl_resource_get_user_data(resource);

    wl_list_remove(wl_resource_get_link(resource));

    if (!pointer_constraints->input_focus) {
        ww_assert(!pointer_constraints->remote_pointer);
        return;
    }

    if (pointer_constraints->remote_pointer) {
        struct wl_client *focus_client =
            wl_resource_get_client(pointer_constraints->input_focus->surface->resource);
        if (wl_resource_get_client(resource) == focus_client) {
            zwp_locked_pointer_v1_destroy(pointer_constraints->remote_pointer);
            pointer_constraints->remote_pointer = NULL;

            wl_signal_emit_mutable(&pointer_constraints->server->events.pointer_unlock, NULL);
        }
    }
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
    struct server_pointer_constraints *pointer_constraints = wl_resource_get_user_data(resource);

    struct wl_resource *locked_pointer_resource = wl_resource_create(
        client, &zwp_locked_pointer_v1_interface, wl_resource_get_version(resource), id);
    if (!locked_pointer_resource) {
        wl_resource_post_no_memory(resource);
        return;
    }
    wl_resource_set_implementation(locked_pointer_resource, &locked_pointer_impl,
                                   pointer_constraints, locked_pointer_resource_destroy);

    struct wl_client *focus_client =
        wl_resource_get_client(pointer_constraints->input_focus->surface->resource);
    if (client == focus_client) {
        if (!pointer_constraints->remote_pointer) {
            pointer_constraints->remote_pointer = zwp_pointer_constraints_v1_lock_pointer(
                pointer_constraints->remote, pointer_constraints->server->ui.surface,
                server_get_wl_pointer(pointer_constraints->server), NULL,
                ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
            ww_assert(pointer_constraints->remote_pointer);

            wl_signal_emit_mutable(&pointer_constraints->server->events.pointer_lock, NULL);
        }
    }

    wl_list_insert(&pointer_constraints->objects, wl_resource_get_link(locked_pointer_resource));
}

static const struct zwp_pointer_constraints_v1_interface pointer_constraints_impl = {
    .confine_pointer = pointer_constraints_confine_pointer,
    .destroy = pointer_constraints_destroy,
    .lock_pointer = pointer_constraints_lock_pointer,
};

static void
on_global_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SRV_POINTER_CONSTRAINTS_VERSION);

    struct server_pointer_constraints *pointer_constraints = data;

    struct wl_resource *resource =
        wl_resource_create(client, &zwp_pointer_constraints_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &pointer_constraints_impl, pointer_constraints,
                                   pointer_constraints_resource_destroy);
}

static void
on_input_focus(struct wl_listener *listener, void *data) {
    struct server_pointer_constraints *pointer_constraints =
        wl_container_of(listener, pointer_constraints, on_input_focus);
    struct server_view *view = data;

    bool was_locked = !!pointer_constraints->remote_pointer;
    bool is_locked = false;

    if (view) {
        struct wl_client *focus_client = wl_resource_get_client(view->surface->resource);

        struct wl_resource *resource;
        wl_resource_for_each(resource, &pointer_constraints->objects) {
            if (wl_resource_get_client(resource) != focus_client) {
                continue;
            }

            is_locked = true;
            break;
        }
    }

    if (was_locked && !is_locked) {
        ww_assert(pointer_constraints->remote_pointer);

        zwp_locked_pointer_v1_destroy(pointer_constraints->remote_pointer);
        pointer_constraints->remote_pointer = NULL;

        wl_signal_emit_mutable(&pointer_constraints->server->events.pointer_unlock, NULL);
    } else if (!was_locked && is_locked) {
        ww_assert(!pointer_constraints->remote_pointer);

        pointer_constraints->remote_pointer = zwp_pointer_constraints_v1_lock_pointer(
            pointer_constraints->remote, pointer_constraints->server->ui.surface,
            server_get_wl_pointer(pointer_constraints->server), NULL,
            ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        ww_assert(pointer_constraints->remote_pointer);

        wl_signal_emit_mutable(&pointer_constraints->server->events.pointer_lock, NULL);
    }

    pointer_constraints->input_focus = view;
}

static void
on_pointer(struct wl_listener *listener, void *data) {
    struct server_pointer_constraints *pointer_constraints =
        wl_container_of(listener, pointer_constraints, on_pointer);
    struct wl_pointer *pointer = server_get_wl_pointer(pointer_constraints->server);

    if (pointer_constraints->remote_pointer) {
        zwp_locked_pointer_v1_destroy(pointer_constraints->remote_pointer);
        pointer_constraints->remote_pointer = zwp_pointer_constraints_v1_lock_pointer(
            pointer_constraints->remote, pointer_constraints->server->ui.surface, pointer, NULL,
            ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        ww_assert(pointer_constraints->remote_pointer);
    }
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_pointer_constraints *pointer_constraints =
        wl_container_of(listener, pointer_constraints, on_display_destroy);

    wl_global_destroy(pointer_constraints->global);

    if (pointer_constraints->remote_pointer) {
        zwp_locked_pointer_v1_destroy(pointer_constraints->remote_pointer);
    }

    wl_list_remove(&pointer_constraints->on_input_focus.link);
    wl_list_remove(&pointer_constraints->on_pointer.link);
    wl_list_remove(&pointer_constraints->on_display_destroy.link);

    free(pointer_constraints);
}

struct server_pointer_constraints *
server_pointer_constraints_create(struct server *server) {
    struct server_pointer_constraints *pointer_constraints =
        calloc(1, sizeof(*pointer_constraints));
    if (!pointer_constraints) {
        ww_log(LOG_ERROR, "failed to allocate server_pointer_constraints");
        return NULL;
    }

    pointer_constraints->server = server;

    pointer_constraints->global =
        wl_global_create(server->display, &zwp_pointer_constraints_v1_interface,
                         SRV_POINTER_CONSTRAINTS_VERSION, pointer_constraints, on_global_bind);
    if (!pointer_constraints->global) {
        ww_log(LOG_ERROR, "failed to allocate wl_pointer_constraints global");
        free(pointer_constraints);
        return NULL;
    }

    wl_list_init(&pointer_constraints->objects);

    pointer_constraints->remote = server->backend.pointer_constraints;

    pointer_constraints->on_input_focus.notify = on_input_focus;
    wl_signal_add(&server->events.input_focus, &pointer_constraints->on_input_focus);

    pointer_constraints->on_pointer.notify = on_pointer;
    wl_signal_add(&server->backend.events.seat_pointer, &pointer_constraints->on_pointer);

    pointer_constraints->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &pointer_constraints->on_display_destroy);

    return pointer_constraints;
}

void
server_pointer_constraints_set_hint(struct server_pointer_constraints *pointer_constraints,
                                    double x, double y) {
    ww_assert(pointer_constraints->remote_pointer);

    zwp_locked_pointer_v1_set_cursor_position_hint(
        pointer_constraints->remote_pointer, wl_fixed_from_double(x), wl_fixed_from_double(y));
}
