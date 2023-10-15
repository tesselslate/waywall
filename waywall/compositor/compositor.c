/*
 *  The compositor module ties the other compositor submodules together and provides a public API
 *  for the wall modules to use.
 */

#define WAYWALL_COMPOSITOR_IMPL

#include "compositor/compositor.h"
#include "compositor/input.h"
#include "compositor/pub_window_util.h"
#include "compositor/render.h"
#include "compositor/xwayland.h"
#include "pointer-constraints-unstable-v1-protocol.h"
#include "relative-pointer-unstable-v1-protocol.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/wayland.h>
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>

// TODO: X11 backend support (will need adjustments in input+render)

static void
on_registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
                   uint32_t version) {
    struct compositor *compositor = data;

    if (strcmp(interface, wl_seat_interface.name) == 0) {
        if (compositor->remote.seat) {
            wlr_log(WLR_INFO, "multiple seats advertised by compositor");
            return;
        }

        compositor->remote.seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
        ww_assert(compositor->remote.seat);

        compositor->remote.pointer = wl_seat_get_pointer(compositor->remote.seat);
        ww_assert(compositor->remote.pointer);
    } else if (strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) {
        compositor->remote.constraints =
            wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1);
        ww_assert(compositor->remote.constraints);
    } else if (strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
        compositor->remote.relative_pointer_manager =
            wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1);
        ww_assert(compositor->remote.relative_pointer_manager);
    }
}

static void
on_registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    // TODO
}

static const struct wl_registry_listener registry_listener = {
    .global = on_registry_global,
    .global_remove = on_registry_global_remove,
};

static void
on_window_destroy(struct wl_listener *listener, void *data) {
    struct compositor *compositor = wl_container_of(listener, compositor, on_window_destroy);
    if (compositor->should_stop && wl_list_length(&compositor->xwl->windows) == 0) {
        wl_display_terminate(compositor->display);
    }
}

/*
 *  Public API
 */

struct compositor *
compositor_create(struct compositor_config config) {
    struct compositor *compositor = calloc(1, sizeof(struct compositor));
    if (!compositor) {
        wlr_log(WLR_ERROR, "failed to allocate compositor");
        return NULL;
    }
    compositor->config = config;

    compositor->display = wl_display_create();
    if (!compositor->display) {
        wlr_log(WLR_ERROR, "failed to create wl_display");
        goto cleanup;
    }

    compositor->backend_headless = wlr_headless_backend_create(compositor->display);
    if (!compositor->backend_headless) {
        wlr_log(WLR_ERROR, "failed to create headless backend");
        goto cleanup;
    }
    wlr_headless_add_output(compositor->backend_headless, HEADLESS_WIDTH, HEADLESS_HEIGHT);

    compositor->backend_wl = wlr_wl_backend_create(compositor->display, NULL);
    if (!compositor->backend_wl) {
        wlr_log(WLR_ERROR, "failed to create wayland backend");
        goto cleanup;
    }
    wlr_wl_output_create(compositor->backend_wl);

    compositor->remote.display = wlr_wl_backend_get_remote_display(compositor->backend_wl);
    ww_assert(compositor->remote.display);

    compositor->remote.registry = wl_display_get_registry(compositor->remote.display);
    wl_registry_add_listener(compositor->remote.registry, &registry_listener, compositor);
    wl_display_roundtrip(compositor->remote.display);
    if (!compositor->remote.pointer) {
        wlr_log(WLR_ERROR, "failed to get remote wayland pointer");
        goto cleanup;
    }
    if (!compositor->remote.relative_pointer_manager) {
        wlr_log(WLR_ERROR, "failed to get remote relative pointer manager");
        goto cleanup;
    }
    if (!compositor->remote.constraints) {
        wlr_log(WLR_ERROR, "failed to get remote pointer constraints");
        goto cleanup;
    }

    compositor->remote.relative_pointer = zwp_relative_pointer_manager_v1_get_relative_pointer(
        compositor->remote.relative_pointer_manager, compositor->remote.pointer);
    ww_assert(compositor->remote.relative_pointer);

    compositor->backend = wlr_multi_backend_create(compositor->display);
    if (!compositor->backend) {
        wlr_log(WLR_ERROR, "failed to create multi backend");
        goto cleanup;
    }
    if (!wlr_multi_backend_add(compositor->backend, compositor->backend_headless)) {
        wlr_log(WLR_ERROR, "failed to add headless backend");
        goto cleanup;
    }
    if (!wlr_multi_backend_add(compositor->backend, compositor->backend_wl)) {
        wlr_log(WLR_ERROR, "failed to add wayland backend");
        goto cleanup;
    }

    compositor->renderer = wlr_renderer_autocreate(compositor->backend);
    if (!compositor->renderer) {
        wlr_log(WLR_ERROR, "failed to create renderer");
        goto cleanup;
    }
    wlr_renderer_init_wl_display(compositor->renderer, compositor->display);

    compositor->allocator = wlr_allocator_autocreate(compositor->backend, compositor->renderer);
    if (!compositor->allocator) {
        wlr_log(WLR_ERROR, "failed to create allocator");
        goto cleanup;
    }

    compositor->compositor = wlr_compositor_create(compositor->display, 5, compositor->renderer);
    if (!compositor->compositor) {
        wlr_log(WLR_ERROR, "failed to create wlr_compositor");
        goto cleanup;
    }
    if (!wlr_subcompositor_create(compositor->display)) {
        wlr_log(WLR_ERROR, "failed to create subcompositor");
        goto cleanup;
    }

    compositor->dmabuf_export = wlr_export_dmabuf_manager_v1_create(compositor->display);
    if (!compositor->dmabuf_export) {
        wlr_log(WLR_ERROR, "failed to create export_dmabuf_manager");
        goto cleanup;
    }

    compositor->xwl = xwl_create(compositor);
    if (!compositor->xwl) {
        wlr_log(WLR_ERROR, "failed to create comp_xwayland");
        goto cleanup;
    }

    compositor->on_window_destroy.notify = on_window_destroy;
    wl_signal_add(&compositor->xwl->events.window_destroy, &compositor->on_window_destroy);

    compositor->render = render_create(compositor);
    if (!compositor->render) {
        wlr_log(WLR_ERROR, "failed to create comp_render");
        goto cleanup;
    }

    compositor->input = input_create(compositor);
    if (!compositor->input) {
        wlr_log(WLR_ERROR, "failed to create comp_input");
        goto cleanup;
    }

    xwl_update_cursor(compositor->xwl);
    wlr_xwayland_set_seat(compositor->xwl->xwayland, compositor->input->seat);

    return compositor;

cleanup:
    compositor_destroy(compositor);
    return NULL;
}

void
compositor_destroy(struct compositor *compositor) {
    if (compositor->render) {
        render_destroy(compositor->render);
    }
    if (compositor->xwl) {
        xwl_destroy(compositor->xwl);
    }
    if (compositor->allocator) {
        wlr_allocator_destroy(compositor->allocator);
    }
    if (compositor->renderer) {
        wlr_renderer_destroy(compositor->renderer);
    }
    if (compositor->remote.relative_pointer) {
        zwp_relative_pointer_v1_destroy(compositor->remote.relative_pointer);
    }
    if (compositor->remote.relative_pointer_manager) {
        zwp_relative_pointer_manager_v1_destroy(compositor->remote.relative_pointer_manager);
    }
    if (compositor->remote.constraints) {
        zwp_pointer_constraints_v1_destroy(compositor->remote.constraints);
    }
    if (compositor->remote.pointer) {
        wl_pointer_destroy(compositor->remote.pointer);
    }
    if (compositor->remote.seat) {
        wl_seat_destroy(compositor->remote.seat);
    }
    if (compositor->remote.registry) {
        wl_registry_destroy(compositor->remote.registry);
    }
    if (compositor->backend) {
        wlr_backend_destroy(compositor->backend);
    } else {
        if (compositor->backend_headless) {
            wlr_backend_destroy(compositor->backend_headless);
        }
        if (compositor->backend_wl) {
            wlr_backend_destroy(compositor->backend_wl);
        }
    }
    if (compositor->input) {
        input_destroy(compositor->input);
    }
    if (compositor->display) {
        wl_display_destroy(compositor->display);
    }
    free(compositor);
}

struct wl_event_loop *
compositor_get_loop(struct compositor *compositor) {
    return wl_display_get_event_loop(compositor->display);
}

void
compositor_load_config(struct compositor *compositor, struct compositor_config config) {
    render_load_config(compositor->render, config);
    input_load_config(compositor->input, config);

    if (config.stop_on_close && !compositor->render->wl) {
        wlr_log(WLR_INFO, "stop on close enabled with new configuration - stopping");
        wl_display_terminate(compositor->display);
    }

    compositor->config = config;
}

bool
compositor_run(struct compositor *compositor, int display_file_fd) {
    if (!wlr_backend_start(compositor->backend)) {
        wlr_backend_destroy(compositor->backend);
        return false;
    }

    const char *socket = wl_display_add_socket_auto(compositor->display);
    if (!socket) {
        wlr_backend_destroy(compositor->backend);
        return false;
    }
    setenv("WAYLAND_DISPLAY", socket, true);
    setenv("DISPLAY", compositor->xwl->xwayland->display_name, true);
    char buf[256];
    ssize_t len =
        snprintf(buf, ARRAY_LEN(buf), "%s\n%s", socket, compositor->xwl->xwayland->display_name);
    if (len >= (ssize_t)ARRAY_LEN(buf) || len < 0) {
        wlr_log(WLR_ERROR, "failed to write waywall-display file (%zd)", len);
        return false;
    }
    if (write(display_file_fd, buf, len) == -1) {
        wlr_log_errno(WLR_ERROR, "failed to write waywall-display");
        return false;
    }
    if (ftruncate(display_file_fd, len) == -1) {
        wlr_log_errno(WLR_ERROR, "failed to truncate waywall-display");
        return false;
    }

    wl_display_run(compositor->display);
    return true;
}

void
compositor_stop(struct compositor *compositor) {
    if (compositor->should_stop) {
        wlr_log(WLR_INFO, "received 2nd stop call - terminating");
        wl_display_terminate(compositor->display);
        return;
    }

    compositor->should_stop = true;
    if (wl_list_length(&compositor->render->windows) == 0) {
        wl_display_terminate(compositor->display);
        return;
    }

    struct window *window;
    wl_list_for_each (window, &compositor->render->windows, link) {
        window_close(window);
    }
}
