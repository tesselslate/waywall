#include "server/wl_output.h"
#include "server/server.h"
#include "server/ui.h"
#include "util/alloc.h"
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-util.h>

#define SRV_OUTPUT_VERSION 4

static void
output_resource_destroy(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
}

static void
output_release(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct wl_output_interface output_impl = {
    .release = output_release,
};

static void
on_global_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SRV_OUTPUT_VERSION);

    struct server_output *output = data;

    struct wl_resource *resource = wl_resource_create(client, &wl_output_interface, version, id);
    check_alloc(resource);
    wl_resource_set_implementation(resource, &output_impl, output, output_resource_destroy);

    wl_output_send_geometry(resource, 0, 0, 0, 0, WL_OUTPUT_SUBPIXEL_UNKNOWN, "waywall", "waywall",
                            WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT, output->ui->width, output->ui->height, 0);

    if (version >= WL_OUTPUT_NAME_SINCE_VERSION) {
        wl_output_send_name(resource, "waywall output");
    }
    if (version >= WL_OUTPUT_DESCRIPTION_SINCE_VERSION) {
        wl_output_send_description(resource, "waywall output");
    }

    wl_list_insert(&output->objects, wl_resource_get_link(resource));
}

static void
on_resize(struct wl_listener *listener, void *data) {
    struct server_output *output = wl_container_of(listener, output, on_resize);

    struct wl_resource *output_resource, *tmp;
    wl_resource_for_each_safe(output_resource, tmp, &output->objects) {
        wl_output_send_mode(output_resource, WL_OUTPUT_MODE_CURRENT, output->ui->width,
                            output->ui->height, 0);

        if (wl_resource_get_version(output_resource) >= WL_OUTPUT_DONE_SINCE_VERSION) {
            wl_output_send_done(output_resource);
        }
    }
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_output *output = wl_container_of(listener, output, on_display_destroy);

    wl_global_destroy(output->global);

    wl_list_remove(&output->on_resize.link);
    wl_list_remove(&output->on_display_destroy.link);

    free(output);
}

struct server_output *
server_output_create(struct server *server, struct server_ui *ui) {
    struct server_output *output = zalloc(1, sizeof(*output));

    output->global = wl_global_create(server->display, &wl_output_interface, SRV_OUTPUT_VERSION,
                                      output, on_global_bind);
    check_alloc(output->global);

    wl_list_init(&output->objects);
    output->ui = ui;

    output->on_resize.notify = on_resize;
    wl_signal_add(&ui->events.resize, &output->on_resize);

    output->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &output->on_display_destroy);

    return output;
}
