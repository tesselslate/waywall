#include "server/wp_presentation.h"
#include "config/config.h"
#include "presentation-time-client-protocol.h"
#include "presentation-time-server-protocol.h"
#include "server/backend.h"
#include "server/server.h"
#include "server/surface.h"
#include "server/ui.h"
#include "util/alloc.h"
#include "util/prelude.h"
#include <stdint.h>
#include <stdlib.h>

static constexpr int SRV_PRESENTATION_VERSION = 2;

struct server_presentation_feedback {
    struct wl_resource *resource;
    struct wp_presentation_feedback *remote;
};

static void
on_presentation_feedback_discarded(void *data, struct wp_presentation_feedback *wl) {
    struct server_presentation_feedback *presentation_feedback = data;

    wp_presentation_feedback_send_discarded(presentation_feedback->resource);
}

static void
on_presentation_feedback_presented(void *data, struct wp_presentation_feedback *wl,
                                   uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec,
                                   uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo,
                                   enum wp_presentation_feedback_kind kind) {
    struct server_presentation_feedback *presentation_feedback = data;

    wp_presentation_feedback_send_presented(presentation_feedback->resource, tv_sec_hi, tv_sec_lo,
                                            tv_nsec, refresh, seq_hi, seq_lo, kind);
}

static void
on_presentation_feedback_sync_output(void *data, struct wp_presentation_feedback *wl,
                                     struct wl_output *output) {
    // Unused.
}

static const struct wp_presentation_feedback_listener presentation_feedback_listener = {
    .discarded = on_presentation_feedback_discarded,
    .presented = on_presentation_feedback_presented,
    .sync_output = on_presentation_feedback_sync_output,
};

static void
presentation_feedback_resource_destroy(struct wl_resource *resource) {
    struct server_presentation_feedback *presentation_feedback =
        wl_resource_get_user_data(resource);

    wp_presentation_feedback_destroy(presentation_feedback->remote);
    free(presentation_feedback);
}

static void
presentation_resource_destroy(struct wl_resource *resource) {
    // Unused.
}

static void
presentation_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
presentation_feedback(struct wl_client *client, struct wl_resource *resource,
                      struct wl_resource *surface_resource, uint32_t id) {
    struct server_presentation *presentation = wl_resource_get_user_data(resource);

    struct server_surface *surface = server_surface_from_resource(surface_resource);

    struct server_presentation_feedback *presentation_feedback =
        zalloc(1, sizeof(*presentation_feedback));

    presentation_feedback->remote = wp_presentation_feedback(presentation->remote, surface->remote);
    check_alloc(presentation_feedback->remote);
    wp_presentation_feedback_add_listener(presentation_feedback->remote,
                                          &presentation_feedback_listener, presentation_feedback);

    struct wl_resource *presentation_feedback_resource = wl_resource_create(
        client, &wp_presentation_feedback_interface, wl_resource_get_version(resource), id);
    check_alloc(presentation_feedback_resource);
    wl_resource_set_implementation(presentation_feedback_resource, nullptr, presentation_feedback,
                                   presentation_feedback_resource_destroy);

    presentation_feedback->resource = presentation_feedback_resource;
}

static const struct wp_presentation_interface presentation_impl = {
    .destroy = presentation_destroy,
    .feedback = presentation_feedback,
};

static void
on_global_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SRV_PRESENTATION_VERSION);

    struct server_presentation *presentation = data;

    struct wl_resource *resource =
        wl_resource_create(client, &wp_presentation_interface, version, id);
    check_alloc(resource);
    wl_resource_set_implementation(resource, &presentation_impl, presentation,
                                   presentation_resource_destroy);

    wp_presentation_send_clock_id(resource, presentation->remote_clock_id);
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_presentation *presentation =
        wl_container_of(listener, presentation, on_display_destroy);

    wl_global_destroy(presentation->global);
    wl_list_remove(&presentation->on_display_destroy.link);

    free(presentation);
}

struct server_presentation *
server_presentation_create(struct server *server) {
    struct server_presentation *presentation = zalloc(1, sizeof(*presentation));

    presentation->remote = server->backend->presentation;
    presentation->remote_clock_id = server->backend->presentation_clock_id;

    presentation->global = wl_global_create(server->display, &wp_presentation_interface,
                                            SRV_PRESENTATION_VERSION, presentation, on_global_bind);
    check_alloc(presentation->global);

    presentation->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &presentation->on_display_destroy);

    return presentation;
}
