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

/*
 * This module essentially implements a state machine where the pointer is either locked or
 * unlocked. The pointer is only locked when a client (i.e. Minecraft) has a valid locked pointer
 * object and has input focus. Otherwise, the pointer is unlocked.
 *
 * If the user has the input.confine_pointer option enabled in their configuration, then the pointer
 * should be confined to the waywall window whenever the pointer is unlocked.
 *
 * Finally, the remote wl_pointer which is being locked/confined may disappear or change at any
 * point due to changes in seat capabilities. This means that active confined_pointer/locked_pointer
 * objects must be destroyed and recreated whenever the available wl_pointer changes. As a result,
 * server_pointer_constraints.locked can be true even while locked_pointer is nullptr. Both
 * locked_pointer and confined_pointer may also be nullptr at the same time.
 */

static constexpr int SRV_POINTER_CONSTRAINTS_VERSION = 1;

#define resource_eq(a, b) ((a) == (b))

struct constraint_data {
    struct server_pointer_constraints *parent;
    struct wl_resource *surface;
};

static bool
constraint_exists(struct server_pointer_constraints *pointer_constraints,
                  struct wl_resource *surface_resource) {
    struct wl_resource *resource;
    wl_list_for_each (resource, &pointer_constraints->obj_locked, link) {
        struct constraint_data *data = wl_resource_get_user_data(resource);
        if (resource_eq(data->surface, surface_resource)) {
            return true;
        }
    }

    return false;
}

static void
lock_pointer(struct server_pointer_constraints *pointer_constraints, struct wl_pointer *pointer) {
    if (pointer_constraints->confined_pointer) {
        zwp_confined_pointer_v1_destroy(pointer_constraints->confined_pointer);
        pointer_constraints->confined_pointer = nullptr;
        wl_surface_commit(pointer_constraints->server->ui->root.surface);
    }

    if (pointer && !pointer_constraints->locked_pointer) {
        pointer_constraints->locked_pointer = zwp_pointer_constraints_v1_lock_pointer(
            pointer_constraints->remote, pointer_constraints->server->ui->root.surface, pointer,
            nullptr, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        check_alloc(pointer_constraints->locked_pointer);

        zwp_locked_pointer_v1_set_cursor_position_hint(
            pointer_constraints->locked_pointer, wl_fixed_from_double(pointer_constraints->hint.x),
            wl_fixed_from_double(pointer_constraints->hint.y));
        wl_surface_commit(pointer_constraints->server->ui->root.surface);
    }

    if (!pointer_constraints->locked) {
        wl_signal_emit_mutable(&pointer_constraints->server->events.pointer_lock, nullptr);
    }
    pointer_constraints->locked = true;
}

static void
unlock_pointer(struct server_pointer_constraints *pointer_constraints, struct wl_pointer *pointer) {
    if (pointer_constraints->locked_pointer) {
        zwp_locked_pointer_v1_destroy(pointer_constraints->locked_pointer);
        pointer_constraints->locked_pointer = nullptr;
        wl_surface_commit(pointer_constraints->server->ui->root.surface);
    }

    bool should_confine =
        pointer_constraints->config.confine && pointer && !pointer_constraints->confined_pointer;
    if (should_confine) {
        pointer_constraints->confined_pointer = zwp_pointer_constraints_v1_confine_pointer(
            pointer_constraints->remote, pointer_constraints->server->ui->root.surface, pointer,
            nullptr, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        check_alloc(pointer_constraints->confined_pointer);
        wl_surface_commit(pointer_constraints->server->ui->root.surface);
    }

    if (pointer_constraints->locked) {
        wl_signal_emit_mutable(&pointer_constraints->server->events.pointer_unlock, nullptr);
    }
    pointer_constraints->locked = false;
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
    struct constraint_data *constraint_data = wl_resource_get_user_data(resource);
    struct server_pointer_constraints *pointer_constraints = constraint_data->parent;
    struct wl_resource *surface_resource = constraint_data->surface;

    wl_list_remove(wl_resource_get_link(resource));
    free(constraint_data);

    // Check if the destroyed locked_pointer object belonged to the view with input focus. If so,
    // the pointer should be unlocked.
    if (!pointer_constraints->input_focus) {
        ww_assert(!pointer_constraints->locked);
        return;
    }

    bool should_unlock =
        pointer_constraints->locked && pointer_constraints->input_focus &&
        resource_eq(pointer_constraints->input_focus->surface->resource, surface_resource);
    if (should_unlock) {
        unlock_pointer(pointer_constraints, server_get_wl_pointer(pointer_constraints->server));
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

    if (constraint_exists(pointer_constraints, surface_resource)) {
        wl_resource_post_error(resource, ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED,
                               "surface already has pointer constraint");
        return;
    }

    // TODO: confined_pointer objects should also count for whether or not constraints exist, but we
    // don't really care about them at all.

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

    if (constraint_exists(pointer_constraints, surface_resource)) {
        wl_resource_post_error(resource, ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED,
                               "surface already has pointer constraint");
        return;
    }

    struct constraint_data *data = zalloc(1, sizeof(*data));
    data->parent = pointer_constraints;
    data->surface = surface_resource;

    struct wl_resource *locked_pointer_resource = wl_resource_create(
        client, &zwp_locked_pointer_v1_interface, wl_resource_get_version(resource), id);
    check_alloc(locked_pointer_resource);
    wl_resource_set_implementation(locked_pointer_resource, &locked_pointer_impl, data,
                                   locked_pointer_resource_destroy);

    // Check if the new pointer lock belongs to the view with input focus. If so, the pointer should
    // be locked.
    bool should_lock =
        pointer_constraints->input_focus &&
        resource_eq(surface_resource, pointer_constraints->input_focus->surface->resource);
    if (should_lock) {
        lock_pointer(pointer_constraints, server_get_wl_pointer(pointer_constraints->server));
    }

    wl_list_insert(&pointer_constraints->obj_locked, wl_resource_get_link(locked_pointer_resource));
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
    struct wl_pointer *pointer = server_get_wl_pointer(pointer_constraints->server);

    pointer_constraints->input_focus = view;

    // If there is no focused view then there can be no active pointer lock.
    if (!view) {
        unlock_pointer(pointer_constraints, pointer);
        return;
    }

    struct wl_resource *target_surface = pointer_constraints->input_focus->surface->resource;
    struct wl_resource *resource;
    wl_resource_for_each(resource, &pointer_constraints->obj_locked) {
        struct constraint_data *constraint_data = wl_resource_get_user_data(resource);
        if (resource_eq(constraint_data->surface, target_surface)) {
            lock_pointer(pointer_constraints, pointer);
            return;
        }
    }

    // If the newly focused view doesn't have a pointer lock then the pointer should be unlocked.
    unlock_pointer(pointer_constraints, pointer);
}

static void
on_pointer(struct wl_listener *listener, void *data) {
    struct server_pointer_constraints *pointer_constraints =
        wl_container_of(listener, pointer_constraints, on_pointer);
    struct wl_pointer *pointer = server_get_wl_pointer(pointer_constraints->server);

    if (pointer_constraints->locked) {
        lock_pointer(pointer_constraints, pointer);
    } else {
        unlock_pointer(pointer_constraints, pointer);
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

    wl_list_init(&pointer_constraints->obj_locked);

    pointer_constraints->remote = server->backend->pointer_constraints;

    pointer_constraints->on_input_focus.notify = on_input_focus;
    wl_signal_add(&server->events.input_focus, &pointer_constraints->on_input_focus);

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
    pointer_constraints->config.confine = confine;
    if (!pointer_constraints->locked) {
        unlock_pointer(pointer_constraints, server_get_wl_pointer(pointer_constraints->server));
    }
}

void
server_pointer_constraints_set_hint(struct server_pointer_constraints *pointer_constraints,
                                    double x, double y) {
    if (pointer_constraints->locked_pointer) {
        zwp_locked_pointer_v1_set_cursor_position_hint(
            pointer_constraints->locked_pointer, wl_fixed_from_double(x), wl_fixed_from_double(y));
    }

    pointer_constraints->hint.x = x;
    pointer_constraints->hint.y = y;
}
