#include "server/wp_pointer_constraints.h"
#include "config/config.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-server-protocol.h"
#include "server/backend.h"
#include "server/server.h"
#include "server/ui.h"
#include "server/wl_compositor.h"
#include "util/alloc.h"
#include "util/prelude.h"
#include <stdint.h>
#include <stdlib.h>

#define SRV_POINTER_CONSTRAINTS_VERSION 1

static void
lock_pointer(struct server_pointer_constraints *pointer_constraints) {
    if (pointer_constraints->config.confine) {
        if (pointer_constraints->confined_pointer) {
            zwp_confined_pointer_v1_destroy(pointer_constraints->confined_pointer);
            pointer_constraints->confined_pointer = NULL;
        }
    }

    pointer_constraints->locked_pointer = zwp_pointer_constraints_v1_lock_pointer(
        pointer_constraints->remote, pointer_constraints->server->ui->root.surface,
        server_get_wl_pointer(pointer_constraints->server), NULL,
        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    check_alloc(pointer_constraints->locked_pointer);

    wl_signal_emit_mutable(&pointer_constraints->server->events.pointer_lock, NULL);
}

static void
unlock_pointer(struct server_pointer_constraints *pointer_constraints) {
    zwp_locked_pointer_v1_destroy(pointer_constraints->locked_pointer);
    pointer_constraints->locked_pointer = NULL;

    wl_signal_emit_mutable(&pointer_constraints->server->events.pointer_unlock, NULL);

    if (pointer_constraints->config.confine) {
        pointer_constraints->confined_pointer = zwp_pointer_constraints_v1_confine_pointer(
            pointer_constraints->remote, pointer_constraints->server->ui->root.surface,
            server_get_wl_pointer(pointer_constraints->server), NULL,
            ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        check_alloc(pointer_constraints->confined_pointer);
    }
}

static void
confined_pointer_resource_destroy(struct wl_resource *resource) {
    // Unused.
}

static void
confined_pointer_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
confined_pointer_set_region(struct wl_client *client, struct wl_resource *resource,
                            struct wl_resource *region_resource) {
    // Unused. We don't care about what region Xwayland wants to confine the pointer to.
}

static const struct zwp_confined_pointer_v1_interface confined_pointer_impl = {
    .destroy = confined_pointer_destroy,
    .set_region = confined_pointer_set_region,
};

static void
locked_pointer_resource_destroy(struct wl_resource *resource) {
    struct server_pointer_constraints *pointer_constraints = wl_resource_get_user_data(resource);

    wl_list_remove(wl_resource_get_link(resource));

    if (!pointer_constraints->input_focus) {
        ww_assert(!pointer_constraints->locked_pointer);
        return;
    }

    if (pointer_constraints->locked_pointer) {
        struct wl_client *focus_client =
            wl_resource_get_client(pointer_constraints->input_focus->surface->resource);
        if (wl_resource_get_client(resource) == focus_client) {
            unlock_pointer(pointer_constraints);
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
    // Unfortunately, Xwayland may attempt to confine the pointer. We don't want to respect it, but
    // we do need to at least create the confined pointer resource.
    struct server_pointer_constraints *pointer_constraints = wl_resource_get_user_data(resource);

    struct wl_resource *confined_pointer_resource = wl_resource_create(
        client, &zwp_confined_pointer_v1_interface, wl_resource_get_version(resource), id);
    check_alloc(confined_pointer_resource);
    wl_resource_set_implementation(confined_pointer_resource, &confined_pointer_impl,
                                   pointer_constraints, confined_pointer_resource_destroy);
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
    check_alloc(locked_pointer_resource);
    wl_resource_set_implementation(locked_pointer_resource, &locked_pointer_impl,
                                   pointer_constraints, locked_pointer_resource_destroy);

    if (pointer_constraints->input_focus) {
        struct wl_client *focus_client =
            wl_resource_get_client(pointer_constraints->input_focus->surface->resource);
        if (client == focus_client) {
            if (!pointer_constraints->locked_pointer) {
                lock_pointer(pointer_constraints);
            }
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
    check_alloc(resource);
    wl_resource_set_implementation(resource, &pointer_constraints_impl, pointer_constraints,
                                   pointer_constraints_resource_destroy);
}

static void
on_input_focus(struct wl_listener *listener, void *data) {
    struct server_pointer_constraints *pointer_constraints =
        wl_container_of(listener, pointer_constraints, on_input_focus);
    struct server_view *view = data;

    bool was_locked = !!pointer_constraints->locked_pointer;
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
        ww_assert(pointer_constraints->locked_pointer);

        unlock_pointer(pointer_constraints);
    } else if (!was_locked && is_locked) {
        ww_assert(!pointer_constraints->locked_pointer);

        lock_pointer(pointer_constraints);
    }

    pointer_constraints->input_focus = view;
}

static void
on_map_status(struct wl_listener *listener, void *data) {
    struct server_pointer_constraints *pointer_constraints =
        wl_container_of(listener, pointer_constraints, on_map_status);

    bool mapped = *(bool *)data;

    if (mapped) {
        server_pointer_constraints_set_confine(pointer_constraints,
                                               pointer_constraints->config.confine);
    } else {
        if (pointer_constraints->confined_pointer) {
            zwp_confined_pointer_v1_destroy(pointer_constraints->confined_pointer);
            pointer_constraints->confined_pointer = NULL;
        }
        if (pointer_constraints->locked_pointer) {
            zwp_locked_pointer_v1_destroy(pointer_constraints->locked_pointer);
            pointer_constraints->locked_pointer = NULL;
        }
    }
}

static void
on_pointer(struct wl_listener *listener, void *data) {
    struct server_pointer_constraints *pointer_constraints =
        wl_container_of(listener, pointer_constraints, on_pointer);
    struct wl_pointer *pointer = server_get_wl_pointer(pointer_constraints->server);

    if (pointer_constraints->locked_pointer) {
        zwp_locked_pointer_v1_destroy(pointer_constraints->locked_pointer);
        pointer_constraints->locked_pointer = zwp_pointer_constraints_v1_lock_pointer(
            pointer_constraints->remote, pointer_constraints->server->ui->root.surface, pointer,
            NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        check_alloc(pointer_constraints->locked_pointer);
    } else if (pointer_constraints->confined_pointer) {
        zwp_confined_pointer_v1_destroy(pointer_constraints->confined_pointer);
        pointer_constraints->confined_pointer = zwp_pointer_constraints_v1_confine_pointer(
            pointer_constraints->remote, pointer_constraints->server->ui->root.surface, pointer,
            NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        check_alloc(pointer_constraints->confined_pointer);
    }
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_pointer_constraints *pointer_constraints =
        wl_container_of(listener, pointer_constraints, on_display_destroy);

    wl_global_destroy(pointer_constraints->global);

    if (pointer_constraints->locked_pointer) {
        zwp_locked_pointer_v1_destroy(pointer_constraints->locked_pointer);
    }
    if (pointer_constraints->confined_pointer) {
        zwp_confined_pointer_v1_destroy(pointer_constraints->confined_pointer);
    }

    wl_list_remove(&pointer_constraints->on_input_focus.link);
    wl_list_remove(&pointer_constraints->on_map_status.link);
    wl_list_remove(&pointer_constraints->on_pointer.link);
    wl_list_remove(&pointer_constraints->on_display_destroy.link);

    free(pointer_constraints);
}

struct server_pointer_constraints *
server_pointer_constraints_create(struct server *server, struct config *cfg) {
    struct server_pointer_constraints *pointer_constraints =
        zalloc(1, sizeof(*pointer_constraints));

    pointer_constraints->server = server;

    pointer_constraints->global =
        wl_global_create(server->display, &zwp_pointer_constraints_v1_interface,
                         SRV_POINTER_CONSTRAINTS_VERSION, pointer_constraints, on_global_bind);
    check_alloc(pointer_constraints->global);

    wl_list_init(&pointer_constraints->objects);

    pointer_constraints->remote = server->backend->pointer_constraints;

    pointer_constraints->on_input_focus.notify = on_input_focus;
    wl_signal_add(&server->events.input_focus, &pointer_constraints->on_input_focus);

    pointer_constraints->on_map_status.notify = on_map_status;
    wl_signal_add(&server->events.map_status, &pointer_constraints->on_map_status);

    pointer_constraints->on_pointer.notify = on_pointer;
    wl_signal_add(&server->backend->events.seat_pointer, &pointer_constraints->on_pointer);

    pointer_constraints->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &pointer_constraints->on_display_destroy);

    pointer_constraints->config.confine = cfg->input.confine;
    return pointer_constraints;
}

void
server_pointer_constraints_set_confine(struct server_pointer_constraints *pointer_constraints,
                                       bool confine) {
    bool confined = !!pointer_constraints->confined_pointer;
    bool locked = !!pointer_constraints->locked_pointer;

    if (!confine && confined) {
        zwp_confined_pointer_v1_destroy(pointer_constraints->confined_pointer);
        pointer_constraints->confined_pointer = NULL;
    } else if (confine && (!confined && !locked)) {
        pointer_constraints->confined_pointer = zwp_pointer_constraints_v1_confine_pointer(
            pointer_constraints->remote, pointer_constraints->server->ui->root.surface,
            server_get_wl_pointer(pointer_constraints->server), NULL,
            ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        check_alloc(pointer_constraints->confined_pointer);
    }

    pointer_constraints->config.confine = confine;
}

void
server_pointer_constraints_set_hint(struct server_pointer_constraints *pointer_constraints,
                                    double x, double y) {
    ww_assert(pointer_constraints->locked_pointer);

    zwp_locked_pointer_v1_set_cursor_position_hint(
        pointer_constraints->locked_pointer, wl_fixed_from_double(x), wl_fixed_from_double(y));
}
