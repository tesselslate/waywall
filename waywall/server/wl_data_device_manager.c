#define _GNU_SOURCE

#include "server/wl_data_device_manager.h"
#include "server/backend.h"
#include "server/server.h"
#include "server/ui.h"
#include "server/wl_compositor.h"
#include "util.h"
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server.h>

#define SRV_DATA_DEVICE_MANAGER_VERSION 1

struct mime_type {
    struct wl_list link;
    char *data;
};

static const struct wl_data_offer_interface data_offer_impl;
static void data_offer_resource_destroy(struct wl_resource *resource);

static struct server_data_offer *
create_selection_offer(struct server_data_device *data_device,
                       struct server_data_source *data_source) {
    struct server_data_offer *data_offer = zalloc(1, sizeof(*data_offer));
    data_offer->device = data_device;
    data_offer->source = data_source;

    data_offer->resource =
        wl_resource_create(wl_resource_get_client(data_device->resource), &wl_data_offer_interface,
                           wl_resource_get_version(data_device->resource), 0);
    check_alloc(data_offer->resource);
    wl_resource_set_implementation(data_offer->resource, &data_offer_impl, data_offer,
                                   data_offer_resource_destroy);

    wl_list_insert(&data_source->offers, &data_offer->link);

    return data_offer;
}

static void
send_selection_offer(struct server_data_offer *offer) {
    struct server_data_device *data_device = offer->device;

    wl_data_device_send_data_offer(data_device->resource, offer->resource);

    struct mime_type *mime_type;
    wl_list_for_each (mime_type, &offer->source->mime_types, link) {
        wl_data_offer_send_offer(offer->resource, mime_type->data);
    }

    wl_data_device_send_selection(data_device->resource, offer->resource);
}

static void
data_offer_resource_destroy(struct wl_resource *resource) {
    struct server_data_offer *data_offer = wl_resource_get_user_data(resource);

    wl_list_remove(&data_offer->link);
    free(data_offer);
}

static void
data_offer_accept(struct wl_client *client, struct wl_resource *resource, uint32_t serial,
                  const char *mime_type) {
    wl_client_post_implementation_error(client, "wl_data_offer.accept is not implemented");
}

static void
data_offer_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
data_offer_receive(struct wl_client *client, struct wl_resource *resource, const char *mime_type,
                   int32_t fd) {
    struct server_data_offer *data_offer = wl_resource_get_user_data(resource);
    if (!data_offer->source) {
        wl_client_post_implementation_error(
            client, "wl_data_offer.receive cannot be called on invalidated offers");
        return;
    }

    ww_log(LOG_INFO, "receive %s %d", mime_type, fd);

    wl_data_source_send_send(data_offer->source->resource, mime_type, fd);
    close(fd);
}

static const struct wl_data_offer_interface data_offer_impl = {
    .accept = data_offer_accept,
    .destroy = data_offer_destroy,
    .receive = data_offer_receive,
};

static void
data_device_resource_destroy(struct wl_resource *resource) {
    struct server_data_device *data_device = wl_resource_get_user_data(resource);

    wl_list_remove(&data_device->link);

    free(data_device);
}

static void
data_device_set_selection(struct wl_client *client, struct wl_resource *resource,
                          struct wl_resource *data_source_resource, uint32_t serial) {
    struct server_data_device *src_device = wl_resource_get_user_data(resource);
    struct server_data_device_manager *data_device_manager = src_device->parent;

    if (data_device_manager->selection.source) {
        wl_resource_destroy(data_device_manager->selection.source->resource);
    }

    // A NULL wl_data_source can be provided to unset the selection.
    if (!data_source_resource) {
        return;
    }

    struct server_data_source *data_source = wl_resource_get_user_data(data_source_resource);
    if (data_source->prepared) {
        wl_resource_post_error(resource, WL_DATA_DEVICE_ERROR_USED_SOURCE,
                               "cannot reuse wl_data_source for wl_data_device.set_selection");
        return;
    }
    data_source->prepared = true;

    struct wl_client *focus_client = NULL;
    if (data_device_manager->input_focus) {
        focus_client = wl_resource_get_client(data_device_manager->input_focus->surface->resource);
    }

    struct server_data_device *data_device;
    wl_list_for_each (data_device, &data_device_manager->devices, link) {
        // The new selection offer should be sent to the client with keyboard focus.
        if (wl_resource_get_client(data_device->resource) != focus_client) {
            continue;
        }

        struct server_data_offer *data_offer = create_selection_offer(data_device, data_source);
        send_selection_offer(data_offer);
    }
}

static void
data_device_start_drag(struct wl_client *client, struct wl_resource *resource,
                       struct wl_resource *data_source_resource,
                       struct wl_resource *origin_surface_resource,
                       struct wl_resource *icon_surface_resource, uint32_t serial) {
    wl_client_post_implementation_error(client, "wl_data_device.start_drag is not implemented");
}

static const struct wl_data_device_interface data_device_impl = {
    .set_selection = data_device_set_selection,
    .start_drag = data_device_start_drag,
};

static void
data_source_resource_destroy(struct wl_resource *resource) {
    struct server_data_source *data_source = wl_resource_get_user_data(resource);

    struct mime_type *mime_type, *tmp;
    wl_list_for_each_safe (mime_type, tmp, &data_source->mime_types, link) {
        wl_list_remove(&mime_type->link);
        free(mime_type->data);
        free(mime_type);
    }

    struct server_data_offer *data_offer;
    wl_list_for_each (data_offer, &data_source->offers, link) {
        data_offer->source = NULL;
    }

    struct server_data_device_manager *data_device_manager = data_source->parent;
    if (data_device_manager->selection.source == data_source) {
        wl_data_source_send_cancelled(data_source->resource);
    }

    free(data_source);
}

static void
data_source_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
data_source_offer(struct wl_client *client, struct wl_resource *resource, const char *type_str) {
    struct server_data_source *data_source = wl_resource_get_user_data(resource);

    if (data_source->prepared) {
        wl_client_post_implementation_error(client,
                                            "wl_data_source.offer called on prepared data source");
        return;
    }

    struct mime_type *mime_type = zalloc(1, sizeof(*mime_type));
    mime_type->data = strdup(type_str);
    check_alloc(mime_type->data);

    wl_list_insert(&data_source->mime_types, &mime_type->link);
}

static const struct wl_data_source_interface data_source_impl = {
    .destroy = data_source_destroy,
    .offer = data_source_offer,
};

static void
data_device_manager_resource_destroy(struct wl_resource *resource) {
    // Unused.
}

static void
data_device_manager_create_data_source(struct wl_client *client, struct wl_resource *resource,
                                       uint32_t id) {
    struct server_data_device_manager *data_device_manager = wl_resource_get_user_data(resource);

    struct server_data_source *data_source = zalloc(1, sizeof(*data_source));
    data_source->resource = wl_resource_create(client, &wl_data_source_interface,
                                               wl_resource_get_version(resource), id);
    check_alloc(data_source->resource);
    wl_resource_set_implementation(data_source->resource, &data_source_impl, data_source,
                                   data_source_resource_destroy);

    data_source->parent = data_device_manager;
    wl_list_init(&data_source->mime_types);
    wl_list_init(&data_source->offers);
}

static void
data_device_manager_get_data_device(struct wl_client *client, struct wl_resource *resource,
                                    uint32_t id, struct wl_resource *seat_resource) {
    struct server_data_device_manager *data_device_manager = wl_resource_get_user_data(resource);

    struct server_data_device *data_device = zalloc(1, sizeof(*data_device));
    data_device->resource = wl_resource_create(client, &wl_data_device_interface,
                                               wl_resource_get_version(resource), id);
    check_alloc(data_device->resource);
    wl_resource_set_implementation(data_device->resource, &data_device_impl, data_device,
                                   data_device_resource_destroy);

    data_device->parent = data_device_manager;

    wl_list_insert(&data_device_manager->devices, &data_device->link);
}

static const struct wl_data_device_manager_interface data_device_manager_impl = {
    .create_data_source = data_device_manager_create_data_source,
    .get_data_device = data_device_manager_get_data_device,
};

static void
on_global_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SRV_DATA_DEVICE_MANAGER_VERSION);

    struct server_data_device_manager *data_device_manager = data;

    struct wl_resource *resource =
        wl_resource_create(client, &wl_data_device_manager_interface, version, id);
    check_alloc(resource);
    wl_resource_set_implementation(resource, &data_device_manager_impl, data_device_manager,
                                   data_device_manager_resource_destroy);
}

static void
on_input_focus(struct wl_listener *listener, void *data) {
    struct server_data_device_manager *data_device_manager =
        wl_container_of(listener, data_device_manager, on_input_focus);
    struct server_view *view = data;

    data_device_manager->input_focus = view;

    if (!view) {
        return;
    }

    struct wl_client *client = wl_resource_get_client(view->surface->resource);
    struct server_data_device *data_device;
    wl_list_for_each (data_device, &data_device_manager->devices, link) {
        if (wl_resource_get_client(data_device->resource) != client) {
            continue;
        }

        if (data_device_manager->selection.source) {
            struct server_data_offer *data_offer =
                create_selection_offer(data_device, data_device_manager->selection.source);
            send_selection_offer(data_offer);
        } else {
            wl_data_device_send_selection(data_device->resource, NULL);
        }
    }
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_data_device_manager *data_device_manager =
        wl_container_of(listener, data_device_manager, on_display_destroy);

    wl_global_destroy(data_device_manager->global);

    wl_list_remove(&data_device_manager->on_input_focus.link);
    wl_list_remove(&data_device_manager->on_display_destroy.link);

    free(data_device_manager);
}

struct server_data_device_manager *
server_data_device_manager_create(struct server *server) {
    struct server_data_device_manager *data_device_manager =
        zalloc(1, sizeof(*data_device_manager));

    data_device_manager->global =
        wl_global_create(server->display, &wl_data_device_manager_interface,
                         SRV_DATA_DEVICE_MANAGER_VERSION, data_device_manager, on_global_bind);
    check_alloc(data_device_manager->global);

    data_device_manager->remote.manager = server->backend->data_device_manager;
    // TODO: Do anything at all on the remote connection

    data_device_manager->on_input_focus.notify = on_input_focus;
    wl_signal_add(&server->events.input_focus, &data_device_manager->on_input_focus);

    data_device_manager->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &data_device_manager->on_display_destroy);

    wl_list_init(&data_device_manager->devices);

    return data_device_manager;
}
