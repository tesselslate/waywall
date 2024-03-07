#include "server/ui.h"
#include "server/remote_buffer.h"
#include "server/server.h"
#include "server/wl_compositor.h"
#include "util.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <wayland-client.h>

#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 480

static void
on_xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel) {
    struct server_ui *ui = data;

    if (!ui->mapped) {
        ww_log(LOG_WARN, "received spurious xdg_toplevel.close from remote compositor");
        return;
    }
    server_ui_hide(ui);

    // TODO: more useful close logic
}

static void
on_xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel, int32_t width,
                          int32_t height, struct wl_array *states) {
    struct server_ui *ui = data;

    if (width > 0) {
        ui->width = width;
    } else if (ui->width == 0) {
        ui->width = DEFAULT_WIDTH;
    }

    if (height > 0) {
        ui->height = height;
    } else if (ui->height == 0) {
        ui->height = DEFAULT_HEIGHT;
    }
}

static void
on_xdg_toplevel_configure_bounds(void *data, struct xdg_toplevel *xdg_toplevel, int32_t width,
                                 int32_t height) {
    // Unused.
}

static void
on_xdg_toplevel_wm_capabilities(void *data, struct xdg_toplevel *xdg_toplevel,
                                struct wl_array *capabilities) {
    // Unused.
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .close = on_xdg_toplevel_close,
    .configure = on_xdg_toplevel_configure,
    .configure_bounds = on_xdg_toplevel_configure_bounds,
    .wm_capabilities = on_xdg_toplevel_wm_capabilities,
};

static void
on_xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
    struct server_ui *ui = data;

    xdg_surface_set_window_geometry(xdg_surface, 0, 0, ui->width, ui->height);
    wp_viewport_set_destination(ui->viewport, ui->width, ui->height);

    xdg_surface_ack_configure(xdg_surface, serial);
    wl_surface_commit(ui->surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = on_xdg_surface_configure,
};

void
server_ui_destroy(struct server_ui *ui) {
    xdg_toplevel_destroy(ui->xdg_toplevel);
    xdg_surface_destroy(ui->xdg_surface);
    wp_viewport_destroy(ui->viewport);
    wl_surface_destroy(ui->surface);
    remote_buffer_deref(ui->background);
}

int
server_ui_init(struct server *server, struct server_ui *ui) {
    ui->server = server;

    // TODO: configurable background
    ui->background = remote_buffer_manager_color(server->remote_buf, (uint8_t[4]){0, 0, 0, 255});
    if (!ui->background) {
        ww_log(LOG_ERROR, "failed to create root buffer");
        return 1;
    }

    ui->surface = wl_compositor_create_surface(server->backend.compositor);
    if (!ui->surface) {
        ww_log(LOG_ERROR, "failed to create root surface");
        goto fail_surface;
    }

    ui->viewport = wp_viewporter_get_viewport(server->backend.viewporter, ui->surface);
    if (!ui->viewport) {
        ww_log(LOG_ERROR, "failed to create root viewport");
        goto fail_viewport;
    }

    ui->xdg_surface = xdg_wm_base_get_xdg_surface(server->backend.xdg_wm_base, ui->surface);
    if (!ui->xdg_surface) {
        ww_log(LOG_ERROR, "failed to create root xdg surface");
        goto fail_xdg_surface;
    }
    xdg_surface_add_listener(ui->xdg_surface, &xdg_surface_listener, ui);

    ui->xdg_toplevel = xdg_surface_get_toplevel(ui->xdg_surface);
    if (!ui->xdg_toplevel) {
        ww_log(LOG_ERROR, "failed to create root xdg toplevel");
        goto fail_xdg_toplevel;
    }
    xdg_toplevel_add_listener(ui->xdg_toplevel, &xdg_toplevel_listener, ui);

    wl_list_init(&ui->views);

    return 0;

fail_xdg_toplevel:
    xdg_surface_destroy(ui->xdg_surface);

fail_xdg_surface:
    wp_viewport_destroy(ui->viewport);

fail_viewport:
    wl_surface_destroy(ui->surface);

fail_surface:
    remote_buffer_deref(ui->background);
    return 1;
}

void
server_ui_hide(struct server_ui *ui) {
    ww_assert(ui->mapped);

    wl_surface_attach(ui->surface, NULL, 0, 0);
    wl_surface_commit(ui->surface);

    ui->mapped = false;
}

void
server_ui_show(struct server_ui *ui) {
    ww_assert(!ui->mapped);

    struct wl_display *display = ui->server->backend.display;

    wl_surface_attach(ui->surface, NULL, 0, 0);
    wl_surface_commit(ui->surface);
    wl_display_roundtrip(display);

    wl_surface_attach(ui->surface, ui->background, 0, 0);
    wl_surface_commit(ui->surface);
    wl_display_roundtrip(display);

    xdg_toplevel_set_title(ui->xdg_toplevel, "waywall");
    xdg_toplevel_set_app_id(ui->xdg_toplevel, "waywall");

    ui->mapped = true;
}

void
server_view_set_crop(struct server_view *view, double x, double y, double width, double height) {
    wp_viewport_set_source(view->viewport, wl_fixed_from_double(x), wl_fixed_from_double(y),
                           wl_fixed_from_double(width), wl_fixed_from_double(height));
    wl_surface_commit(view->surface->remote);
}

void
server_view_set_dest_size(struct server_view *view, uint32_t width, uint32_t height) {
    wp_viewport_set_destination(view->viewport, width, height);
}

void
server_view_set_position(struct server_view *view, int32_t x, int32_t y) {
    wl_subsurface_set_position(view->subsurface, x, y);
    wl_surface_commit(view->surface->remote);

    view->x = x;
    view->y = y;
}

void
server_view_set_size(struct server_view *view, uint32_t width, uint32_t height) {
    view->impl->set_size(view->impl_resource, width, height);
}

void
server_view_unset_crop(struct server_view *view) {
    const wl_fixed_t minusone = wl_fixed_from_int(-1);
    wp_viewport_set_source(view->viewport, minusone, minusone, minusone, minusone);
    wl_surface_commit(view->surface->remote);
}

struct server_view *
server_view_create(struct server_ui *ui, struct server_surface *surface,
                   const struct server_view_impl *impl, struct wl_resource *impl_resource) {
    struct server_view *view = calloc(1, sizeof(*view));
    if (!view) {
        return NULL;
    }

    view->surface = surface;
    view->subsurface = wl_subcompositor_get_subsurface(ui->server->backend.subcompositor,
                                                       surface->remote, ui->surface);
    if (!view->subsurface) {
        free(view);
        return NULL;
    }
    wl_subsurface_set_desync(view->subsurface);
    wl_surface_commit(surface->remote);
    wl_surface_commit(ui->surface);

    view->viewport = wp_viewporter_get_viewport(ui->server->backend.viewporter, surface->remote);
    if (!view->viewport) {
        wl_subsurface_destroy(view->subsurface);
        free(view);
        return NULL;
    }

    view->impl = impl;
    view->impl_resource = impl_resource;

    wl_list_insert(&ui->views, &view->link);

    return view;
}

void
server_view_destroy(struct server_view *view) {
    wl_subsurface_destroy(view->subsurface);
    wp_viewport_destroy(view->viewport);
    wl_list_remove(&view->link);
    free(view);
}

struct server_view *
server_view_from_surface(struct server_surface *surface) {
    return surface->role->get_view(surface->role_resource);
}
