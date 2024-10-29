#include "server/wl_data_device_manager.h"
#include "server/backend.h"
#include "server/server.h"
#include "server/ui.h"
#include "server/wl_compositor.h"
#include "server/wl_seat.h"
#include "server/xwayland.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <wayland-server-protocol.h>

#define SRV_DATA_DEVICE_MANAGER_VERSION 1

struct mime_type {
    struct wl_list link;
    char *data;
};

struct remote_offer {
    struct wl_data_offer *wl;
    struct wl_list mime_types; // mime_type.link
};

static const struct wl_data_offer_interface data_offer_impl;

static struct server_data_offer *create_local_offer(struct server_data_device *data_device,
                                                    struct server_data_source *data_source);
static struct server_data_offer *create_remote_offer(struct server_data_device *data_device,
                                                     struct remote_offer *remote_offer);
static void data_offer_resource_destroy(struct wl_resource *resource);
static void destroy_previous_selection(struct server_data_device_manager *data_device_manager);
static void remote_offer_destroy(struct remote_offer *offer);
static void selection_content_destroy(struct server_selection_content *selection_content);
static void send_selection_offer(struct server_data_offer *offer);
static void set_selection(struct server_data_device_manager *data_device_manager,
                          enum server_selection_type type, void *data);

static int
handle_selection_pipe(int32_t fd, uint32_t mask, void *data) {
    static const int CHUNK_SIZE = 4096;

    struct server_data_device_manager *data_device_manager = data;
    ww_assert(fd == data_device_manager->selection_content.fd);

    char **str = &data_device_manager->selection_content.data;
    ssize_t new_len = data_device_manager->selection_content.len + CHUNK_SIZE;
    *str = realloc(*str, new_len + 1);
    check_alloc(*str);

    ssize_t n = read(fd, *str + data_device_manager->selection_content.len, CHUNK_SIZE);
    bool eof = (n == 0);
    if (n == -1) {
        ww_log_errno(LOG_ERROR, "failed to read from selection pipe");
        selection_content_destroy(&data_device_manager->selection_content);
        return 0;
    }

    (*str)[data_device_manager->selection_content.len + n] = '\0';
    data_device_manager->selection_content.len = new_len;

    if (eof) {
        ww_log(LOG_INFO, "received selection content of length %lu", strlen(*str));

        xwl_set_clipboard(data_device_manager->server->xwayland, *str);

        wl_event_source_remove(data_device_manager->selection_content.src_pipe);
        data_device_manager->selection_content.src_pipe = NULL;
    }

    return 0;
}

static void
on_data_offer_offer(void *data, struct wl_data_offer *offer, const char *type_str) {
    struct remote_offer *remote_offer = data;

    struct mime_type *mime_type = zalloc(1, sizeof(*mime_type));
    mime_type->data = strdup(type_str);
    check_alloc(mime_type->data);

    wl_list_insert(&remote_offer->mime_types, &mime_type->link);
}

static const struct wl_data_offer_listener data_offer_listener = {
    .offer = on_data_offer_offer,
};

static void
on_data_device_data_offer(void *data, struct wl_data_device *data_device,
                          struct wl_data_offer *offer) {
    struct server_data_device_manager *data_device_manager = data;

    for (size_t i = 0; i < STATIC_ARRLEN(data_device_manager->remote.pending_offers); i++) {
        struct remote_offer **slot = &data_device_manager->remote.pending_offers[i];
        if (*slot) {
            continue;
        }

        struct remote_offer *remote_offer = zalloc(1, sizeof(*remote_offer));
        remote_offer->wl = offer;
        wl_list_init(&remote_offer->mime_types);

        wl_data_offer_add_listener(remote_offer->wl, &data_offer_listener, remote_offer);

        *slot = remote_offer;
        return;
    }

    ww_log(LOG_WARN, "remote compositor is providing too many wl_data_offers");
}

static void
on_data_device_drop(void *data, struct wl_data_device *data_device) {
    // Unused.
}

static void
on_data_device_enter(void *data, struct wl_data_device *data_device, uint32_t serial,
                     struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
                     struct wl_data_offer *offer) {
    struct server_data_device_manager *data_device_manager = data;

    if (data_device_manager->remote.dnd_offer) {
        ww_panic("broken remote compositor - cannot have 2 concurrent DND data offers");
    }

    for (size_t i = 0; i < STATIC_ARRLEN(data_device_manager->remote.pending_offers); i++) {
        struct remote_offer **slot = &data_device_manager->remote.pending_offers[i];

        if (*slot && (*slot)->wl == offer) {
            data_device_manager->remote.dnd_offer = *slot;
            *slot = NULL;
            return;
        }
    }

    ww_log(LOG_WARN, "received wl_data_device.enter with unknown offer");
}

static void
on_data_device_leave(void *data, struct wl_data_device *data_device) {
    struct server_data_device_manager *data_device_manager = data;
    ww_assert(data_device_manager->remote.dnd_offer);

    remote_offer_destroy(data_device_manager->remote.dnd_offer);
    data_device_manager->remote.dnd_offer = NULL;
}

static void
on_data_device_motion(void *data, struct wl_data_device *data_device, uint32_t time, wl_fixed_t x,
                      wl_fixed_t y) {
    // Unused.
}

static void
on_data_device_selection(void *data, struct wl_data_device *wl_data_device,
                         struct wl_data_offer *offer) {
    struct server_data_device_manager *data_device_manager = data;

    // Wayland compositors will helpfully offer up the clipboard content we have just provided in
    // the event that we bound a wl_data_source to the clipboard. In that case, ignore the remote
    // offer.
    if (data_device_manager->remote.source) {
        for (size_t i = 0; i < STATIC_ARRLEN(data_device_manager->remote.pending_offers); i++) {
            struct remote_offer **slot = &data_device_manager->remote.pending_offers[i];

            if (*slot && (*slot)->wl == offer) {
                remote_offer_destroy(*slot);
                *slot = NULL;
                break;
            }
        }
        return;
    }

    destroy_previous_selection(data_device_manager);

    if (!offer) {
        return;
    }

    struct remote_offer *remote_offer = NULL;
    for (size_t i = 0; i < STATIC_ARRLEN(data_device_manager->remote.pending_offers); i++) {
        struct remote_offer **slot = &data_device_manager->remote.pending_offers[i];

        if (*slot && (*slot)->wl == offer) {
            remote_offer = *slot;
            *slot = NULL;
            break;
        }
    }

    if (!remote_offer) {
        ww_log(LOG_WARN, "received wl_data_device.selection with unknown offer");
        return;
    }

    set_selection(data_device_manager, SELECTION_REMOTE, remote_offer);

    if (!data_device_manager->input_focus) {
        return;
    }
    struct wl_client *focus_client =
        wl_resource_get_client(data_device_manager->input_focus->surface->resource);

    struct server_data_device *data_device;
    wl_list_for_each (data_device, &data_device_manager->devices, link) {
        // The new selection offer should be sent to the client with keyboard focus.
        if (wl_resource_get_client(data_device->resource) != focus_client) {
            continue;
        }

        struct server_data_offer *data_offer = create_remote_offer(data_device, remote_offer);
        send_selection_offer(data_offer);
    }
}

static const struct wl_data_device_listener data_device_listener = {
    .data_offer = on_data_device_data_offer,
    .drop = on_data_device_drop,
    .enter = on_data_device_enter,
    .leave = on_data_device_leave,
    .motion = on_data_device_motion,
    .selection = on_data_device_selection,
};

static void
on_data_source_cancelled(void *data, struct wl_data_source *data_source) {
    struct server_data_device_manager *data_device_manager = data;
    if (data_device_manager->remote.source == data_source) {
        data_device_manager->remote.source = NULL;
    }

    wl_data_source_destroy(data_source);
}

static void
on_data_source_send(void *data, struct wl_data_source *data_source, const char *mime_type,
                    int32_t fd) {
    struct server_data_device_manager *data_device_manager = data;

    if (data_device_manager->remote.source != data_source) {
        close(fd);
        return;
    }
    if (data_device_manager->selection.type != SELECTION_LOCAL) {
        ww_log(LOG_WARN, "received wl_data_source.send when no local selection was present");
        close(fd);
        return;
    }

    wl_data_source_send_send(data_device_manager->selection.data.local->resource, mime_type, fd);
    close(fd);
}

static void
on_data_source_target(void *data, struct wl_data_source *data_source, const char *mime_type) {
    ww_log(LOG_ERROR, "received wl_data_source.target on a clipboard source");
}

static const struct wl_data_source_listener data_source_listener = {
    .cancelled = on_data_source_cancelled,
    .send = on_data_source_send,
    .target = on_data_source_target,
};

static struct server_data_offer *
create_local_offer(struct server_data_device *data_device, struct server_data_source *data_source) {
    struct server_data_device_manager *data_device_manager = data_device->parent;
    ww_assert(data_device_manager->selection.type == SELECTION_LOCAL);
    ww_assert(data_device_manager->selection.data.local == data_source);

    struct server_data_offer *data_offer = zalloc(1, sizeof(*data_offer));
    data_offer->parent = data_device;
    data_offer->selection = (struct server_selection){
        .type = SELECTION_LOCAL,
        .data.local = data_source,
        .serial = data_device_manager->selection.serial,
    };

    data_offer->resource =
        wl_resource_create(wl_resource_get_client(data_device->resource), &wl_data_offer_interface,
                           wl_resource_get_version(data_device->resource), 0);
    check_alloc(data_offer->resource);
    wl_resource_set_implementation(data_offer->resource, &data_offer_impl, data_offer,
                                   data_offer_resource_destroy);

    return data_offer;
}

static struct server_data_offer *
create_remote_offer(struct server_data_device *data_device, struct remote_offer *remote_offer) {
    struct server_data_device_manager *data_device_manager = data_device->parent;
    ww_assert(data_device_manager->selection.type == SELECTION_REMOTE);
    ww_assert(data_device_manager->selection.data.remote == remote_offer);

    struct server_data_offer *data_offer = zalloc(1, sizeof(*data_offer));
    data_offer->parent = data_device;
    data_offer->selection = (struct server_selection){
        .type = SELECTION_REMOTE,
        .data.remote = remote_offer,
        .serial = data_device_manager->selection.serial,
    };

    data_offer->resource =
        wl_resource_create(wl_resource_get_client(data_device->resource), &wl_data_offer_interface,
                           wl_resource_get_version(data_device->resource), 0);
    check_alloc(data_offer->resource);
    wl_resource_set_implementation(data_offer->resource, &data_offer_impl, data_offer,
                                   data_offer_resource_destroy);

    return data_offer;
}

static void
destroy_previous_selection(struct server_data_device_manager *data_device_manager) {
    switch (data_device_manager->selection.type) {
    case SELECTION_NONE:
        return;
    case SELECTION_LOCAL:
        wl_data_source_send_cancelled(data_device_manager->selection.data.local->resource);
        break;
    case SELECTION_REMOTE:
        remote_offer_destroy(data_device_manager->selection.data.remote);
        break;
    }

    set_selection(data_device_manager, SELECTION_NONE, NULL);
}

static void
remote_offer_destroy(struct remote_offer *offer) {
    wl_data_offer_destroy(offer->wl);

    struct mime_type *mime_type, *tmp;
    wl_list_for_each_safe (mime_type, tmp, &offer->mime_types, link) {
        wl_list_remove(&mime_type->link);
        free(mime_type->data);
        free(mime_type);
    }

    free(offer);
}

static void
selection_content_destroy(struct server_selection_content *selection_content) {
    if (selection_content->src_pipe) {
        wl_event_source_remove(selection_content->src_pipe);
        selection_content->src_pipe = NULL;
    }

    if (selection_content->data) {
        free(selection_content->data);
        selection_content->data = NULL;
    }

    if (selection_content->fd >= 0) {
        close(selection_content->fd);
        selection_content->fd = -1;
    }

    selection_content->len = 0;
}

static void
send_selection_offer(struct server_data_offer *offer) {
    struct server_data_device *data_device = offer->parent;

    wl_data_device_send_data_offer(data_device->resource, offer->resource);

    struct wl_list *mime_types;
    switch (offer->selection.type) {
    case SELECTION_NONE:
        ww_unreachable();
    case SELECTION_LOCAL:
        mime_types = &offer->selection.data.local->mime_types;
        break;
    case SELECTION_REMOTE:
        mime_types = &offer->selection.data.remote->mime_types;
        break;
    }

    struct mime_type *mime_type;
    wl_list_for_each (mime_type, mime_types, link) {
        wl_data_offer_send_offer(offer->resource, mime_type->data);
    }

    wl_data_device_send_selection(data_device->resource, offer->resource);
}

static void
get_utf8_selection(struct server_data_device_manager *data_device_manager) {
    static const char *UTF8_MIME = "text/plain;charset=utf-8";

    selection_content_destroy(&data_device_manager->selection_content);

    if (data_device_manager->selection.type == SELECTION_NONE) {
        data_device_manager->selection_content.data = strdup("");
        check_alloc(data_device_manager->selection_content.data);
        return;
    }

    switch (data_device_manager->selection.type) {
    case SELECTION_LOCAL: {
        struct server_data_source *data_source = data_device_manager->selection.data.local;

        bool utf8_selection = false;
        struct mime_type *mime;
        wl_list_for_each (mime, &data_source->mime_types, link) {
            if (strcasecmp(UTF8_MIME, mime->data) == 0) {
                utf8_selection = true;
                break;
            }
        }

        if (!utf8_selection) {
            ww_log(LOG_WARN, "no UTF-8 text/plain MIME type in local selection");
            return;
        }

        int pipe_fd[2] = {0};
        if (pipe(pipe_fd) != 0) {
            ww_log_errno(LOG_ERROR, "failed to create pipe for selection content");
            return;
        }

        data_device_manager->selection_content.fd = pipe_fd[0];
        wl_data_source_send_send(data_source->resource, UTF8_MIME, pipe_fd[1]);
        close(pipe_fd[1]);

        break;
    }
    case SELECTION_REMOTE: {
        struct remote_offer *offer = data_device_manager->selection.data.remote;

        bool utf8_selection = false;
        struct mime_type *mime;
        wl_list_for_each (mime, &offer->mime_types, link) {
            if (strcasecmp(UTF8_MIME, mime->data) == 0) {
                utf8_selection = true;
                break;
            }
        }

        if (!utf8_selection) {
            ww_log(LOG_WARN, "no UTF-8 text/plain MIME type in remote selection");
            return;
        }

        int pipe_fd[2] = {0};
        if (pipe(pipe_fd) != 0) {
            ww_log_errno(LOG_ERROR, "failed to create pipe for selection content");
            return;
        }

        data_device_manager->selection_content.fd = pipe_fd[0];
        wl_data_offer_receive(offer->wl, UTF8_MIME, pipe_fd[1]);
        close(pipe_fd[1]);

        break;
    }
    default:
        ww_unreachable();
    }

    data_device_manager->selection_content.src_pipe =
        wl_event_loop_add_fd(wl_display_get_event_loop(data_device_manager->server->display),
                             data_device_manager->selection_content.fd, WL_EVENT_READABLE,
                             handle_selection_pipe, data_device_manager);
    check_alloc(data_device_manager->selection_content.src_pipe);
}

static void
set_selection(struct server_data_device_manager *data_device_manager,
              enum server_selection_type type, void *data) {
    data_device_manager->selection.type = type;
    data_device_manager->selection.serial++;

    // There will never be 18 quintillion clipboard updates taking place.
    ww_assert(data_device_manager->selection.serial < UINT64_MAX);

    switch (type) {
    case SELECTION_NONE:
        data_device_manager->selection.data.none = data;
        break;
    case SELECTION_LOCAL:
        data_device_manager->selection.data.local = data;
        break;
    case SELECTION_REMOTE:
        data_device_manager->selection.data.remote = data;
        break;
    }

    // The content of the selection must be copied to the X11 clipboard.
    get_utf8_selection(data_device_manager);
}

static void
data_offer_resource_destroy(struct wl_resource *resource) {
    struct server_data_offer *data_offer = wl_resource_get_user_data(resource);

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
    struct server_selection *active_selection = &data_offer->parent->parent->selection;

    // Check that the offer is still valid. If it is, get the data. If it isn't, don't send anything
    // and close the pipe immediately.
    if (active_selection->serial != data_offer->selection.serial) {
        close(fd);
        return;
    }

    switch (data_offer->selection.type) {
    case SELECTION_NONE:
        ww_unreachable();
    case SELECTION_LOCAL:
        wl_data_source_send_send(data_offer->selection.data.local->resource, mime_type, fd);
        break;
    case SELECTION_REMOTE:
        wl_data_offer_receive(data_offer->selection.data.remote->wl, mime_type, fd);
        break;
    }

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

    destroy_previous_selection(data_device_manager);

    // A NULL wl_data_source can be provided to unset the selection.
    if (!data_source_resource) {
        wl_data_device_set_selection(data_device_manager->remote.device, NULL,
                                     data_device_manager->server->seat->last_serial);
        return;
    }

    struct server_data_source *data_source = wl_resource_get_user_data(data_source_resource);
    if (data_source->prepared) {
        // There's an error code (wl_data_device.used_source) which is intended to be used for this
        // purpose, but it was merged into the Wayland upstream relatively recently (April 2023.)
        wl_client_post_implementation_error(
            client, "cannot reuse wl_data_source for wl_data_device.set_selection");
        return;
    }
    data_source->prepared = true;

    set_selection(data_device_manager, SELECTION_LOCAL, data_source);

    data_device_manager->remote.source =
        wl_data_device_manager_create_data_source(data_device_manager->remote.manager);
    wl_data_source_add_listener(data_device_manager->remote.source, &data_source_listener,
                                data_device_manager);

    struct mime_type *mime_type;
    wl_list_for_each (mime_type, &data_source->mime_types, link) {
        wl_data_source_offer(data_device_manager->remote.source, mime_type->data);
    }

    wl_data_device_set_selection(data_device_manager->remote.device,
                                 data_device_manager->remote.source,
                                 data_device_manager->server->seat->last_serial);

    if (!data_device_manager->input_focus) {
        return;
    }
    struct wl_client *focus_client =
        wl_resource_get_client(data_device_manager->input_focus->surface->resource);

    struct server_data_device *data_device;
    wl_list_for_each (data_device, &data_device_manager->devices, link) {
        // The new selection offer should be sent to the client with keyboard focus.
        if (wl_resource_get_client(data_device->resource) != focus_client) {
            continue;
        }

        struct server_data_offer *data_offer = create_local_offer(data_device, data_source);
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

    struct server_data_device_manager *data_device_manager = data_source->parent;

    if (data_device_manager->selection.type == SELECTION_LOCAL) {
        if (data_device_manager->selection.data.local == data_source) {
            set_selection(data_device_manager, SELECTION_NONE, NULL);
            wl_data_device_set_selection(data_device_manager->remote.device, NULL,
                                         data_device_manager->server->seat->last_serial);
        }
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

    struct wl_client *focus_client = wl_resource_get_client(view->surface->resource);
    struct server_data_device *data_device;
    wl_list_for_each (data_device, &data_device_manager->devices, link) {
        if (wl_resource_get_client(data_device->resource) != focus_client) {
            continue;
        }

        switch (data_device_manager->selection.type) {
        case SELECTION_NONE:
            wl_data_device_send_selection(data_device->resource, NULL);
            break;
        case SELECTION_LOCAL: {
            struct server_data_offer *data_offer =
                create_local_offer(data_device, data_device_manager->selection.data.local);
            send_selection_offer(data_offer);
            break;
        }
        case SELECTION_REMOTE: {
            struct server_data_offer *data_offer =
                create_remote_offer(data_device, data_device_manager->selection.data.remote);
            send_selection_offer(data_offer);
            break;
        }
        }
    }
}

static void
on_keyboard_leave(struct wl_listener *listener, void *data) {
    struct server_data_device_manager *data_device_manager =
        wl_container_of(listener, data_device_manager, on_keyboard_leave);

    if (data_device_manager->selection.type == SELECTION_REMOTE) {
        remote_offer_destroy(data_device_manager->selection.data.remote);
        set_selection(data_device_manager, SELECTION_NONE, NULL);
    }
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_data_device_manager *data_device_manager =
        wl_container_of(listener, data_device_manager, on_display_destroy);

    selection_content_destroy(&data_device_manager->selection_content);

    if (data_device_manager->selection.type == SELECTION_REMOTE) {
        remote_offer_destroy(data_device_manager->selection.data.remote);
    }

    if (data_device_manager->remote.source) {
        wl_data_source_destroy(data_device_manager->remote.source);
    }
    if (data_device_manager->remote.dnd_offer) {
        remote_offer_destroy(data_device_manager->remote.dnd_offer);
    }
    for (size_t i = 0; i < STATIC_ARRLEN(data_device_manager->remote.pending_offers); i++) {
        struct remote_offer *offer = data_device_manager->remote.pending_offers[i];
        if (offer) {
            remote_offer_destroy(offer);
        }
    }

    wl_global_destroy(data_device_manager->global);

    wl_list_remove(&data_device_manager->on_input_focus.link);
    wl_list_remove(&data_device_manager->on_keyboard_leave.link);
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

    data_device_manager->server = server;

    data_device_manager->remote.manager = server->backend->data_device_manager;
    data_device_manager->remote.device = server_get_wl_data_device(server);
    ww_assert(data_device_manager->remote.device);
    wl_data_device_add_listener(data_device_manager->remote.device, &data_device_listener,
                                data_device_manager);

    // TODO: Listen for data device changes

    data_device_manager->on_input_focus.notify = on_input_focus;
    wl_signal_add(&server->events.input_focus, &data_device_manager->on_input_focus);

    data_device_manager->on_keyboard_leave.notify = on_keyboard_leave;
    wl_signal_add(&server->seat->events.keyboard_leave, &data_device_manager->on_keyboard_leave);

    data_device_manager->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &data_device_manager->on_display_destroy);

    wl_list_init(&data_device_manager->devices);

    return data_device_manager;
}
