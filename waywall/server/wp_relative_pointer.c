#include "server/wp_relative_pointer.h"
#include "config/config.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-server-protocol.h"
#include "server/backend.h"
#include "server/server.h"
#include "server/ui.h"
#include "server/wl_compositor.h"
#include "util.h"

#define SRV_RELATIVE_POINTER_VERSION 1

static void
on_relative_pointer_relative_motion(void *data, struct zwp_relative_pointer_v1 *wl,
                                    uint32_t utime_hi, uint32_t utime_lo, wl_fixed_t dx,
                                    wl_fixed_t dy, wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel) {
    struct server_relative_pointer *relative_pointer = data;

    if (!relative_pointer->input_focus) {
        return;
    }

    // Boat eye relies on precise cursor positioning. Sending relative pointer motion events with
    // non-whole number values will cause boat eye to not work correctly.
    relative_pointer->acc_x += wl_fixed_to_double(dx) * relative_pointer->cfg->input.sens;
    relative_pointer->acc_y += wl_fixed_to_double(dy) * relative_pointer->cfg->input.sens;

    double x = trunc(relative_pointer->acc_x);
    relative_pointer->acc_x -= x;

    double y = trunc(relative_pointer->acc_y);
    relative_pointer->acc_y -= y;

    // I'm not sure if any other compositors would have a reason to send non-whole number motion,
    // but better safe than sorry.
    relative_pointer->acc_x_unaccel += wl_fixed_to_double(dx_unaccel);
    relative_pointer->acc_y_unaccel += wl_fixed_to_double(dy_unaccel);

    double x_unaccel = trunc(relative_pointer->acc_x_unaccel);
    relative_pointer->acc_x_unaccel -= x_unaccel;

    double y_unaccel = trunc(relative_pointer->acc_y_unaccel);
    relative_pointer->acc_y_unaccel -= y;

    struct wl_client *client =
        wl_resource_get_client(relative_pointer->input_focus->surface->resource);
    struct wl_resource *resource;
    wl_resource_for_each(resource, &relative_pointer->objects) {
        if (wl_resource_get_client(resource) != client) {
            continue;
        }

        zwp_relative_pointer_v1_send_relative_motion(
            resource, utime_hi, utime_lo, wl_fixed_from_double(x), wl_fixed_from_double(y),
            wl_fixed_from_double(x_unaccel), wl_fixed_from_double(y_unaccel));
    }
}

static const struct zwp_relative_pointer_v1_listener relative_pointer_listener = {
    .relative_motion = on_relative_pointer_relative_motion,
};

static void
process_pointer(struct server_relative_pointer *relative_pointer, struct wl_pointer *pointer) {
    if (relative_pointer->remote_pointer) {
        zwp_relative_pointer_v1_destroy(relative_pointer->remote_pointer);
    }
    if (pointer) {
        relative_pointer->remote_pointer =
            zwp_relative_pointer_manager_v1_get_relative_pointer(relative_pointer->remote, pointer);
        check_alloc(relative_pointer->remote_pointer);

        zwp_relative_pointer_v1_add_listener(relative_pointer->remote_pointer,
                                             &relative_pointer_listener, relative_pointer);
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
    struct server_relative_pointer *relative_pointer = wl_resource_get_user_data(resource);

    struct wl_resource *relative_pointer_resource = wl_resource_create(
        client, &zwp_relative_pointer_v1_interface, wl_resource_get_version(resource), id);
    check_alloc(relative_pointer_resource);
    wl_resource_set_implementation(relative_pointer_resource, &relative_pointer_impl,
                                   relative_pointer, relative_pointer_resource_destroy);

    wl_list_insert(&relative_pointer->objects, wl_resource_get_link(relative_pointer_resource));
}

static const struct zwp_relative_pointer_manager_v1_interface relative_pointer_manager_impl = {
    .destroy = relative_pointer_manager_destroy,
    .get_relative_pointer = relative_pointer_manager_get_relative_pointer,
};

static void
on_global_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SRV_RELATIVE_POINTER_VERSION);

    struct server_relative_pointer *relative_pointer = data;

    struct wl_resource *resource =
        wl_resource_create(client, &zwp_relative_pointer_manager_v1_interface, version, id);
    check_alloc(resource);
    wl_resource_set_implementation(resource, &relative_pointer_manager_impl, relative_pointer,
                                   relative_pointer_manager_resource_destroy);
}

static void
on_input_focus(struct wl_listener *listener, void *data) {
    struct server_relative_pointer *relative_pointer =
        wl_container_of(listener, relative_pointer, on_input_focus);
    struct server_view *view = data;

    relative_pointer->input_focus = view;
}

static void
on_pointer(struct wl_listener *listener, void *data) {
    struct server_relative_pointer *relative_pointer =
        wl_container_of(listener, relative_pointer, on_pointer);

    process_pointer(relative_pointer, server_get_wl_pointer(relative_pointer->server));
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_relative_pointer *relative_pointer =
        wl_container_of(listener, relative_pointer, on_display_destroy);

    wl_global_destroy(relative_pointer->global);

    if (relative_pointer->remote_pointer) {
        zwp_relative_pointer_v1_destroy(relative_pointer->remote_pointer);
    }

    wl_list_remove(&relative_pointer->on_input_focus.link);
    wl_list_remove(&relative_pointer->on_pointer.link);
    wl_list_remove(&relative_pointer->on_display_destroy.link);

    free(relative_pointer);
}

struct server_relative_pointer *
server_relative_pointer_create(struct server *server, struct config *cfg) {
    struct server_relative_pointer *relative_pointer = zalloc(1, sizeof(*relative_pointer));

    relative_pointer->cfg = cfg;
    relative_pointer->server = server;

    relative_pointer->global =
        wl_global_create(server->display, &zwp_relative_pointer_manager_v1_interface,
                         SRV_RELATIVE_POINTER_VERSION, relative_pointer, on_global_bind);
    check_alloc(relative_pointer->global);

    wl_list_init(&relative_pointer->objects);

    relative_pointer->remote = server->backend->relative_pointer_manager;
    process_pointer(relative_pointer, server_get_wl_pointer(server));

    relative_pointer->on_input_focus.notify = on_input_focus;
    wl_signal_add(&server->events.input_focus, &relative_pointer->on_input_focus);

    relative_pointer->on_pointer.notify = on_pointer;
    wl_signal_add(&server->backend->events.seat_pointer, &relative_pointer->on_pointer);

    relative_pointer->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &relative_pointer->on_display_destroy);

    return relative_pointer;
}
