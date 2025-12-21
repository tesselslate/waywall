#include "server/wl_output.h"
#include "server/server.h"
#include "server/ui.h"
#include "server/xserver.h"
#include "server/xwayland.h"
#include "util/alloc.h"
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-util.h>

// TODO: It might be worth considering fake outputs for all of the real host compositor outputs and
// make ingame fullscreen behave as expected (i.e. make the waywall window fullscreen) instead of
// just trying to ignore it. This would be extremely annoying but would fix some other issues (see
// the HACK comment in on_global_bind).

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

    struct wl_client *xwl_client = output->server->xwayland->xserver->client;
    if (client == xwl_client) {
        // HACK: Mouse motion in Xwayland does not work outside of monitor bounds for some reason..?
        // Not sure what the rationale is but I don't feel like digging through Xwayland code any
        // further. Using ingame fullscreen on X11 still uses the 8k x 8k resolution and it is not
        // fixed by the xdg_shell workaround.
        wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT, 8192, 8192, 0);
    } else {
        wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT, 1, 1, 0);
    }

    if (version >= WL_OUTPUT_NAME_SINCE_VERSION) {
        // The monitor name must not have spaces or else older versions of the game using LWJGL2
        // will fail to boot, since LWJGL2 gets monitor information from parsing the output of
        // `xrandr -q` and does not handle spaces correctly.
        wl_output_send_name(resource, "waywall-output");
    }
    if (version >= WL_OUTPUT_DESCRIPTION_SINCE_VERSION) {
        wl_output_send_description(resource, "waywall-output");
    }

    if (version >= WL_OUTPUT_DONE_SINCE_VERSION) {
        wl_output_send_done(resource);
    }

    wl_list_insert(&output->objects, wl_resource_get_link(resource));
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_output *output = wl_container_of(listener, output, on_display_destroy);

    wl_global_destroy(output->global);

    wl_list_remove(&output->on_display_destroy.link);

    free(output);
}

struct server_output *
server_output_create(struct server *server) {
    struct server_output *output = zalloc(1, sizeof(*output));

    output->global = wl_global_create(server->display, &wl_output_interface, SRV_OUTPUT_VERSION,
                                      output, on_global_bind);
    check_alloc(output->global);

    output->server = server;

    wl_list_init(&output->objects);

    output->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &output->on_display_destroy);

    return output;
}
