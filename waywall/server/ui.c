#include "server/ui.h"
#include "config/config.h"
#include "server/backend.h"
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

    wl_signal_emit(&ui->events.resize, NULL);
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

static inline void
txn_apply_visible(struct transaction_view *txn_view, struct server_view *view,
                  struct server_ui *ui) {
    if (txn_view->visible == !!view->subsurface) {
        return;
    }

    if (txn_view->visible) {
        view->subsurface = wl_subcompositor_get_subsurface(
            ui->server->backend->subcompositor, view->surface->remote, view->ui->surface);
        check_alloc(view->subsurface);

        wl_subsurface_set_position(view->subsurface, view->x, view->y);
        wl_subsurface_set_desync(view->subsurface);
        wl_surface_commit(view->surface->remote);
    } else {
        wl_subsurface_destroy(view->subsurface);
        wl_surface_commit(view->surface->remote);
        view->subsurface = NULL;
    }
}

struct server_ui *
server_ui_create(struct server *server, struct config *cfg) {
    struct server_ui *ui = zalloc(1, sizeof(*ui));

    ui->server = server;

    ui->empty_region = wl_compositor_create_region(server->backend->compositor);
    check_alloc(ui->empty_region);

    ui->background = remote_buffer_manager_color(server->remote_buf, cfg->theme.background);
    if (!ui->background) {
        ww_log(LOG_ERROR, "failed to create root buffer");
        goto fail_background;
    }

    ui->surface = wl_compositor_create_surface(server->backend->compositor);
    check_alloc(ui->surface);

    ui->viewport = wp_viewporter_get_viewport(server->backend->viewporter, ui->surface);
    check_alloc(ui->viewport);

    ui->xdg_surface = xdg_wm_base_get_xdg_surface(server->backend->xdg_wm_base, ui->surface);
    check_alloc(ui->xdg_surface);
    xdg_surface_add_listener(ui->xdg_surface, &xdg_surface_listener, ui);

    ui->xdg_toplevel = xdg_surface_get_toplevel(ui->xdg_surface);
    check_alloc(ui->xdg_toplevel);
    xdg_toplevel_add_listener(ui->xdg_toplevel, &xdg_toplevel_listener, ui);

    wl_list_init(&ui->views);

    wl_signal_init(&ui->events.resize);
    wl_signal_init(&ui->events.view_create);
    wl_signal_init(&ui->events.view_destroy);

    return ui;

fail_background:
    wl_region_destroy(ui->empty_region);
    free(ui);
    return NULL;
}

void
server_ui_destroy(struct server_ui *ui) {
    wl_region_destroy(ui->empty_region);

    xdg_toplevel_destroy(ui->xdg_toplevel);
    xdg_surface_destroy(ui->xdg_surface);
    wp_viewport_destroy(ui->viewport);
    wl_surface_destroy(ui->surface);
    remote_buffer_deref(ui->background);

    free(ui);
}

void
server_ui_hide(struct server_ui *ui) {
    ww_assert(ui->mapped);

    wl_surface_attach(ui->surface, NULL, 0, 0);
    wl_surface_commit(ui->surface);

    ui->mapped = false;
}

int
server_ui_set_config(struct server_ui *ui, struct config *cfg) {
    struct wl_buffer *new_background =
        remote_buffer_manager_color(ui->server->remote_buf, cfg->theme.background);
    if (!new_background) {
        ww_log(LOG_ERROR, "failed to create root buffer");
        return 1;
    }

    wl_surface_attach(ui->surface, new_background, 0, 0);
    wl_surface_commit(ui->surface);
    wl_display_roundtrip(ui->server->backend->display);

    remote_buffer_deref(ui->background);
    ui->background = new_background;

    return 0;
}

void
server_ui_show(struct server_ui *ui) {
    ww_assert(!ui->mapped);

    struct wl_display *display = ui->server->backend->display;

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

pid_t
server_view_get_pid(struct server_view *view) {
    return view->impl->get_pid(view->impl_resource);
}

char *
server_view_get_title(struct server_view *view) {
    return view->impl->get_title(view->impl_resource);
}

struct server_view *
server_view_create(struct server_ui *ui, struct server_surface *surface,
                   const struct server_view_impl *impl, struct wl_resource *impl_resource) {
    struct server_view *view = zalloc(1, sizeof(*view));

    view->ui = ui;
    view->surface = surface;

    view->viewport = wp_viewporter_get_viewport(ui->server->backend->viewporter, surface->remote);
    check_alloc(view->viewport);

    view->impl = impl;
    view->impl_resource = impl_resource;

    wl_list_insert(&ui->views, &view->link);

    wl_signal_init(&view->events.destroy);

    wl_signal_emit_mutable(&ui->events.view_create, view);

    return view;
}

void
server_view_destroy(struct server_view *view) {
    wl_signal_emit_mutable(&view->events.destroy, NULL);
    wl_signal_emit_mutable(&view->ui->events.view_destroy, view);

    if (view->subsurface) {
        wl_subsurface_destroy(view->subsurface);
    }
    wp_viewport_destroy(view->viewport);
    wl_list_remove(&view->link);
    free(view);
}

void
transaction_apply(struct server_ui *ui, struct transaction *txn) {
    // TODO: transaction_apply is not actually atomic because the subsurfaces of each
    // server_view are always in desynchronized mode

    // TODO: not sure if it's worth trying to make window resizes adhere to frame perfection
    // since it would be complicated to try and wait on all of the views + add a timeout to
    // apply the layout anyway. dest_size at least ensures they always have the correct bounds
    // even if they look stretched

    struct transaction_view *txn_view;
    wl_list_for_each (txn_view, &txn->views, link) {
        struct server_view *view = txn_view->view;

        if (txn_view->apply & TXN_VIEW_CROP) {
            wp_viewport_set_source(view->viewport, wl_fixed_from_int(txn_view->crop.x),
                                   wl_fixed_from_int(txn_view->crop.y),
                                   wl_fixed_from_int(txn_view->crop.width),
                                   wl_fixed_from_int(txn_view->crop.height));
        }
        if (txn_view->apply & TXN_VIEW_DEST_SIZE) {
            wp_viewport_set_destination(view->viewport, txn_view->dest_width,
                                        txn_view->dest_height);
        }
        if (txn_view->apply & TXN_VIEW_POS) {
            if (view->subsurface) {
                wl_subsurface_set_position(view->subsurface, txn_view->x, txn_view->y);
            }

            view->x = txn_view->x;
            view->y = txn_view->y;
        }
        if (txn_view->apply & TXN_VIEW_SIZE) {
            view->impl->set_size(view->impl_resource, txn_view->width, txn_view->height);
        }
        if (txn_view->apply & TXN_VIEW_VISIBLE) {
            txn_apply_visible(txn_view, view, ui);
        }

        // TODO: Properly store Z order
        if (txn_view->apply & TXN_VIEW_ABOVE) {
            ww_assert(txn_view->view->subsurface);
            wl_subsurface_place_above(txn_view->view->subsurface, txn_view->above);
        }

        wl_surface_commit(view->surface->remote);
    }

    wl_surface_commit(ui->surface);
}

struct transaction *
transaction_create() {
    struct transaction *txn = zalloc(1, sizeof(*txn));

    wl_list_init(&txn->views);

    return txn;
}

void
transaction_destroy(struct transaction *txn) {
    struct transaction_view *txn_view, *tmp;
    wl_list_for_each_safe (txn_view, tmp, &txn->views, link) {
        wl_list_remove(&txn_view->link);
        free(txn_view);
    }

    free(txn);
}

struct transaction_view *
transaction_get_view(struct transaction *txn, struct server_view *view) {
    struct transaction_view *txn_view = zalloc(1, sizeof(*txn_view));

    txn_view->view = view;

    wl_list_insert(&txn->views, &txn_view->link);

    return txn_view;
}

void
transaction_view_set_above(struct transaction_view *view, struct wl_surface *surface) {
    view->above = surface;
    view->apply |= TXN_VIEW_ABOVE;
}

void
transaction_view_set_crop(struct transaction_view *view, uint32_t x, uint32_t y, uint32_t width,
                          uint32_t height) {
    view->crop.x = x;
    view->crop.y = y;
    view->crop.width = width;
    view->crop.height = height;
    view->apply |= TXN_VIEW_CROP;
}

void
transaction_view_set_dest_size(struct transaction_view *view, uint32_t width, uint32_t height) {
    view->dest_width = width;
    view->dest_height = height;
    view->apply |= TXN_VIEW_DEST_SIZE;
}

void
transaction_view_set_position(struct transaction_view *view, uint32_t x, uint32_t y) {
    view->x = x;
    view->y = y;
    view->apply |= TXN_VIEW_POS;
}

void
transaction_view_set_size(struct transaction_view *view, uint32_t width, uint32_t height) {
    view->width = width;
    view->height = height;
    view->apply |= TXN_VIEW_SIZE;
}

void
transaction_view_set_visible(struct transaction_view *view, bool visible) {
    view->visible = visible;
    view->apply |= TXN_VIEW_VISIBLE;
}

struct ui_rectangle *
ui_rectangle_create(struct server_ui *ui, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                    const uint8_t rgba[static 4]) {
    struct ui_rectangle *rect = zalloc(1, sizeof(*rect));

    rect->parent = ui;
    rect->x = x;
    rect->y = y;

    rect->buffer = remote_buffer_manager_color(ui->server->remote_buf, rgba);
    if (!rect->buffer) {
        ww_log(LOG_ERROR, "failed to get color buffer for ui_rectangle");
        goto fail_buffer;
    }

    rect->surface = wl_compositor_create_surface(ui->server->backend->compositor);
    check_alloc(rect->surface);
    wl_surface_attach(rect->surface, rect->buffer, 0, 0);
    wl_surface_set_input_region(rect->surface, ui->empty_region);

    rect->viewport = wp_viewporter_get_viewport(ui->server->backend->viewporter, rect->surface);
    check_alloc(rect->viewport);
    wp_viewport_set_destination(rect->viewport, width, height);

    return rect;

fail_buffer:
    free(rect);
    return NULL;
}

void
ui_rectangle_destroy(struct ui_rectangle *rect) {
    wl_subsurface_destroy(rect->subsurface);
    wp_viewport_destroy(rect->viewport);
    wl_surface_destroy(rect->surface);
    remote_buffer_deref(rect->buffer);
    free(rect);
}

void
ui_rectangle_set_visible(struct ui_rectangle *rect, bool visible) {
    if (visible == !!rect->subsurface) {
        return;
    }

    if (visible) {
        rect->subsurface = wl_subcompositor_get_subsurface(
            rect->parent->server->backend->subcompositor, rect->surface, rect->parent->surface);
        check_alloc(rect->subsurface);

        wl_subsurface_set_position(rect->subsurface, rect->x, rect->y);
        wl_subsurface_set_desync(rect->subsurface);
        wl_surface_commit(rect->surface);
        wl_surface_commit(rect->parent->surface);
    } else {
        wl_subsurface_destroy(rect->subsurface);
        wl_surface_commit(rect->surface);
        rect->subsurface = NULL;
    }
}
