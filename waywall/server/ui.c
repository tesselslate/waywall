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

static struct transaction_view *
get_txn_view(struct transaction *txn, struct server_view *view) {
    // Fast path: return the last accessed view (for when multiple subsequent ops happen)
    if (txn->last_accessed && txn->last_accessed->view == view) {
        return txn->last_accessed;
    }

    // Slow path: scan the list and add a new view
    struct transaction_view *txn_view;
    wl_list_for_each (txn_view, &txn->views, link) {
        if (txn_view->view == view) {
            txn->last_accessed = txn_view;
            return txn_view;
        }
    }

    txn_view = calloc(1, sizeof(*txn_view));
    if (!txn_view) {
        ww_log(LOG_ERROR, "failed to allocate transaction_view");
        return NULL;
    }

    txn_view->view = view;
    wl_list_insert(&txn->views, &txn_view->link);

    txn->last_accessed = txn_view;
    return txn_view;
}

static inline void
txn_apply_visible(struct transaction_view *txn_view, struct server_view *view,
                  struct server_ui *ui) {
    if (txn_view->visible == !!view->subsurface) {
        return;
    }

    if (txn_view->visible) {
        view->subsurface = wl_subcompositor_get_subsurface(
            ui->server->backend->subcompositor, view->surface->remote, view->ui->surface);
        if (!view->subsurface) {
            ww_log(LOG_ERROR, "failed to allocate view subsurface");
            return;
        }

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
    struct server_ui *ui = calloc(1, sizeof(*ui));
    if (!ui) {
        ww_log(LOG_ERROR, "failed to allocate server_ui");
        return NULL;
    }

    ui->cfg = cfg;
    ui->server = server;

    ui->background = remote_buffer_manager_color(server->remote_buf, ui->cfg->theme.background);
    if (!ui->background) {
        ww_log(LOG_ERROR, "failed to create root buffer");
        goto fail_background;
    }

    ui->surface = wl_compositor_create_surface(server->backend->compositor);
    if (!ui->surface) {
        ww_log(LOG_ERROR, "failed to create root surface");
        goto fail_surface;
    }

    ui->viewport = wp_viewporter_get_viewport(server->backend->viewporter, ui->surface);
    if (!ui->viewport) {
        ww_log(LOG_ERROR, "failed to create root viewport");
        goto fail_viewport;
    }

    ui->xdg_surface = xdg_wm_base_get_xdg_surface(server->backend->xdg_wm_base, ui->surface);
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

    wl_signal_init(&ui->events.resize);
    wl_signal_init(&ui->events.view_create);
    wl_signal_init(&ui->events.view_destroy);

    return ui;

fail_xdg_toplevel:
    xdg_surface_destroy(ui->xdg_surface);

fail_xdg_surface:
    wp_viewport_destroy(ui->viewport);

fail_viewport:
    wl_surface_destroy(ui->surface);

fail_surface:
    remote_buffer_deref(ui->background);

fail_background:
    free(ui);
    return NULL;
}

void
server_ui_destroy(struct server_ui *ui) {
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

void
server_view_hide(struct server_view *view) {
    if (!view->subsurface) {
        return;
    }

    wl_subsurface_destroy(view->subsurface);
    wl_surface_commit(view->surface->remote);
    wl_surface_commit(view->ui->surface);
    view->subsurface = NULL;
}

void
server_view_set_crop(struct server_view *view, double x, double y, double width, double height) {
    wp_viewport_set_source(view->viewport, wl_fixed_from_double(x), wl_fixed_from_double(y),
                           wl_fixed_from_double(width), wl_fixed_from_double(height));
    wl_surface_commit(view->surface->remote);
}

void
server_view_set_dest_size(struct server_view *view, uint32_t width, uint32_t height) {
    ww_assert(width > 0 && height > 0);
    wp_viewport_set_destination(view->viewport, width, height);
}

void
server_view_set_position(struct server_view *view, int32_t x, int32_t y) {
    if (view->subsurface) {
        wl_subsurface_set_position(view->subsurface, x, y);
        wl_surface_commit(view->surface->remote);
        wl_surface_commit(view->ui->surface);
    }

    view->x = x;
    view->y = y;
}

void
server_view_set_size(struct server_view *view, uint32_t width, uint32_t height) {
    view->impl->set_size(view->impl_resource, width, height);
}

void
server_view_show(struct server_view *view) {
    if (view->subsurface) {
        return;
    }

    view->subsurface = wl_subcompositor_get_subsurface(view->ui->server->backend->subcompositor,
                                                       view->surface->remote, view->ui->surface);
    if (!view->subsurface) {
        ww_log(LOG_ERROR, "failed to allocate view subsurface");
        return;
    }

    wl_subsurface_set_position(view->subsurface, view->x, view->y);
    wl_subsurface_set_desync(view->subsurface);
    wl_surface_commit(view->surface->remote);
    wl_surface_commit(view->ui->surface);
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

    view->ui = ui;

    view->surface = surface;

    view->viewport = wp_viewporter_get_viewport(ui->server->backend->viewporter, surface->remote);
    if (!view->viewport) {
        wl_subsurface_destroy(view->subsurface);
        free(view);
        return NULL;
    }

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

int
transaction_apply(struct server_ui *ui, struct transaction *txn) {
    if (txn->failed) {
        return 1;
    }

    // TODO: transaction_apply is not actually atomic because the subsurfaces of each server_view
    // are always in desynchronized mode

    // TODO: not sure if it's worth trying to make window resizes adhere to frame perfection since
    // it would be complicated to try and wait on all of the views + add a timeout to apply the
    // layout anyway. dest_size at least ensures they always have the correct bounds even if they
    // look stretched

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

        wl_surface_commit(view->surface->remote);
    }

    wl_surface_commit(ui->surface);
    return 0;
}

struct transaction *
transaction_create() {
    struct transaction *txn = calloc(1, sizeof(*txn));
    if (!txn) {
        ww_log(LOG_ERROR, "failed to allocate transaction");
        return NULL;
    }

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

void
transaction_set_crop(struct transaction *txn, struct server_view *view, uint32_t x, uint32_t y,
                     uint32_t width, uint32_t height) {
    struct transaction_view *txn_view = get_txn_view(txn, view);
    if (!txn_view) {
        txn->failed = true;
        return;
    }

    txn_view->crop.x = x;
    txn_view->crop.y = y;
    txn_view->crop.width = width;
    txn_view->crop.height = height;
    txn_view->apply |= TXN_VIEW_CROP;
}

void
transaction_set_dest_size(struct transaction *txn, struct server_view *view, uint32_t width,
                          uint32_t height) {
    struct transaction_view *txn_view = get_txn_view(txn, view);
    if (!txn_view) {
        txn->failed = true;
        return;
    }

    txn_view->dest_width = width;
    txn_view->dest_height = height;
    txn_view->apply |= TXN_VIEW_DEST_SIZE;
}

void
transaction_set_position(struct transaction *txn, struct server_view *view, uint32_t x,
                         uint32_t y) {
    struct transaction_view *txn_view = get_txn_view(txn, view);
    if (!txn_view) {
        txn->failed = true;
        return;
    }

    txn_view->x = x;
    txn_view->y = y;
    txn_view->apply |= TXN_VIEW_POS;
}

void
transaction_set_size(struct transaction *txn, struct server_view *view, uint32_t width,
                     uint32_t height) {
    struct transaction_view *txn_view = get_txn_view(txn, view);
    if (!txn_view) {
        txn->failed = true;
        return;
    }

    txn_view->width = width;
    txn_view->height = height;
    txn_view->apply |= TXN_VIEW_SIZE;
}

void
transaction_set_visible(struct transaction *txn, struct server_view *view, bool visible) {
    struct transaction_view *txn_view = get_txn_view(txn, view);
    if (!txn_view) {
        txn->failed = true;
        return;
    }

    txn_view->visible = visible;
    txn_view->apply |= TXN_VIEW_VISIBLE;
}
