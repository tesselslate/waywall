#include "compositor/wl_output.h"
#include "compositor/server.h"
#include "util.h"
#include <wayland-client.h>
#include <wayland-server.h>

/*
 *  Events needed for GLFW:
 *
 *  wl_output.geometry(x, y, physical_width, physical_height)
 *  wl_output.mode(flags, width, height, refresh)
 *  wl_output.done()
 *  wl_output.scale(factor)
 *  wl_output.name(name)
 */

#define VERSION 4

static void
send_output_state(struct server_output *output, struct wl_resource *output_resource) {
    wl_output_send_geometry(output_resource, 0, 0, 0, 0, WL_OUTPUT_SUBPIXEL_NONE, "Waywall",
                            "Waywall", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(output_resource, WL_OUTPUT_MODE_CURRENT, output->width, output->height, 0);
    wl_output_send_scale(output_resource, output->scale_factor);
    wl_output_send_name(output_resource, "Waywall");
    wl_output_send_done(output_resource);
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_output *output = wl_container_of(listener, output, display_destroy);

    ww_assert(wl_list_length(&output->clients) == 0);
    wl_global_destroy(output->global);

    free(output);
}

static void
handle_output_release(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
output_destroy(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
}

static const struct wl_output_interface output_impl = {
    .release = handle_output_release,
};

static void
handle_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    static_assert(WL_OUTPUT_DONE_SINCE_VERSION == WL_OUTPUT_SCALE_SINCE_VERSION,
                  "wl_output.done and wl_output.scale were both introduced in the same version");

    ww_assert(version <= VERSION);
    if (version < WL_OUTPUT_DONE_SINCE_VERSION) {
        wl_client_post_implementation_error(client, "wl_output version < %d is unsupported",
                                            WL_OUTPUT_DONE_SINCE_VERSION);
        return;
    }

    struct server_output *output = data;

    struct wl_resource *resource = wl_resource_create(client, &wl_output_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }

    wl_resource_set_implementation(resource, &output_impl, output, output_destroy);

    wl_list_insert(&output->clients, wl_resource_get_link(resource));

    wl_output_send_name(resource, "Waywall");
    send_output_state(output, resource);
}

struct server_output *
server_output_create(struct server *server) {
    struct server_output *output = calloc(1, sizeof(*output));
    if (!output) {
        LOG(LOG_ERROR, "failed to allocate server_output");
        return NULL;
    }

    // TODO: allow configuring output scale factor
    wl_list_init(&output->clients);

    output->scale_factor = 1;

    output->global =
        wl_global_create(server->display, &wl_output_interface, VERSION, output, handle_bind);

    output->display_destroy.notify = on_display_destroy;

    // TODO: window size change listener (for server_output.{width, height})

    wl_display_add_destroy_listener(server->display, &output->display_destroy);

    return output;
}
