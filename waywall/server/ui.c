#include "server/ui.h"
#include "config/config.h"
#include "server/backend.h"
#include "server/buffer.h"
#include "server/remote_buffer.h"
#include "server/server.h"
#include "server/wl_compositor.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include "viewporter-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <stdlib.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 480
#define TXN_TIMEOUT_MS 100

static bool can_apply_crop(struct server_view *view, struct box crop);
static void destroy_txn(struct server_txn *txn);
static void finalize_txn(struct server_txn *txn);

static int
on_txn_timeout(void *data) {
    struct server_txn *txn = data;

    ww_log(LOG_WARN, "transaction timed out");
    finalize_txn(txn);

    return 0;
}

static void
on_txn_view_commit(struct wl_listener *listener, void *data) {
    struct server_txn_dep *dep = wl_container_of(listener, dep, listener);
    struct server_txn_view *tv = wl_container_of(dep, tv, resize_dep);
    struct server_txn *txn = tv->parent;

    uint32_t size[2];
    server_buffer_get_size(server_surface_next_buffer(tv->view->surface), &size[0], &size[1]);

    if (size[0] == tv->width && size[1] == tv->height) {
        wl_list_remove(&dep->listener.link);
        wl_list_remove(&dep->link);

        if (wl_list_empty(&txn->dependencies)) {
            // If this surface had the last transaction dependency, we cannot call wl_surface.commit
            // in `finalize_txn` because the new buffer will not have been committed yet. The
            // `server_surface.commit` event is emitted before we call `wl_surface.commit` on the
            // remote surface, so the new buffer is still part of the pending state.
            tv->needs_commit = false;

            finalize_txn(txn);
        }
    }
}

static void
on_xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel) {
    struct server_ui *ui = data;

    if (!ui->mapped) {
        ww_log(LOG_WARN, "received spurious xdg_toplevel.close from remote compositor");
        return;
    }

    wl_signal_emit_mutable(&ui->events.close, NULL);
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

    ui->resize = true;
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
    wp_viewport_set_destination(ui->root.viewport, ui->width, ui->height);

    xdg_surface_ack_configure(xdg_surface, serial);

    if (ui->resize) {
        wl_signal_emit(&ui->events.resize, NULL);
        ui->resize = false;
    }
    wl_surface_commit(ui->root.surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = on_xdg_surface_configure,
};

static void
on_view_surface_commit(struct wl_listener *listener, void *data) {
    struct server_view *view = wl_container_of(listener, view, on_surface_commit);

    if (!(view->surface->pending.present & SURFACE_STATE_BUFFER)) {
        return;
    }

    if (can_apply_crop(view, view->state.crop)) {
        wp_viewport_set_source(view->viewport, wl_fixed_from_int(view->state.crop.x),
                               wl_fixed_from_int(view->state.crop.y),
                               wl_fixed_from_int(view->state.crop.width),
                               wl_fixed_from_int(view->state.crop.height));
    } else {
        // Just get rid of the source box entirely if the surface gets sized down so we don't
        // crash.
        wp_viewport_set_source(view->viewport, wl_fixed_from_int(-1), wl_fixed_from_int(-1),
                               wl_fixed_from_int(-1), wl_fixed_from_int(-1));
    }
}

static void
txn_apply_crop(struct server_txn_view *tv, struct server_ui *ui) {
    struct box crop = tv->crop;

    ww_assert(((crop.x == -1) == (crop.y == -1)) == ((crop.width == -1) == (crop.height == -1)));

    if (can_apply_crop(tv->view, crop)) {
        wp_viewport_set_source(tv->view->viewport, wl_fixed_from_int(crop.x),
                               wl_fixed_from_int(crop.y), wl_fixed_from_int(crop.width),
                               wl_fixed_from_int(crop.height));
    }
}

static void
txn_apply_dest_size(struct server_txn_view *tv, struct server_ui *ui) {
    wp_viewport_set_destination(tv->view->viewport, tv->dest_width, tv->dest_height);
}

static void
txn_apply_pos(struct server_txn_view *tv, struct server_ui *ui) {
    ww_assert(tv->view->subsurface);
    wl_subsurface_set_position(tv->view->subsurface, tv->x, tv->y);
}

static void
txn_apply_visible(struct server_txn_view *tv, struct server_ui *ui) {
    if (tv->visible == !!tv->view->subsurface) {
        return;
    }

    if (tv->visible) {
        tv->view->subsurface =
            wl_subcompositor_get_subsurface(ui->server->backend->subcompositor,
                                            tv->view->surface->remote, tv->view->ui->tree.surface);
        check_alloc(tv->view->subsurface);

        wl_subsurface_set_position(tv->view->subsurface, tv->view->state.x, tv->view->state.y);
        wl_subsurface_set_desync(tv->view->subsurface);
    } else {
        wl_subsurface_destroy(tv->view->subsurface);
        tv->view->subsurface = NULL;
    }
}

static void
txn_apply_z(struct server_txn_view *tv, struct server_ui *ui) {
    ww_assert(tv->view->subsurface);
    wl_subsurface_place_above(tv->view->subsurface, tv->above);
}

static bool
can_apply_crop(struct server_view *view, struct box crop) {
    ww_assert(((crop.x == -1) == (crop.y == -1)) == ((crop.width == -1) == (crop.height == -1)));

    if (crop.x == -1) {
        return true;
    }

    struct server_buffer *buffer = server_surface_next_buffer(view->surface);
    ww_assert(buffer);

    uint32_t width, height;
    server_buffer_get_size(buffer, &width, &height);

    return (crop.x + crop.width <= (int32_t)width) && (crop.y + crop.height <= (int32_t)height);
}

static void
destroy_txn(struct server_txn *txn) {
    // There may be remaining dependencies if the transaction did not complete.
    struct server_txn_dep *dep, *tmp_dep;
    wl_list_for_each_safe (dep, tmp_dep, &txn->dependencies, link) {
        wl_list_remove(&dep->listener.link);
        wl_list_remove(&dep->link);
    }

    // If the transaction did not complete, we also need to unset the inflight transaction.
    if (txn->ui) {
        if (txn->ui->inflight_txn == txn) {
            txn->ui->inflight_txn = NULL;
        }
    }

    struct server_txn_view *tv, *tmp_view;
    wl_list_for_each_safe (tv, tmp_view, &txn->views, link) {
        wl_list_remove(&tv->link);
        free(tv);
    }

    if (txn->timer) {
        wl_event_source_remove(txn->timer);
    }

    wl_subsurface_set_desync(txn->ui->tree.subsurface);
    free(txn);
}

static void
finalize_txn(struct server_txn *txn) {
    struct server_txn_view *tv;
    wl_list_for_each (tv, &txn->views, link) {
        if (tv->apply & TXN_VIEW_ABOVE) {
            txn_apply_z(tv, txn->ui);
        }
        if (tv->apply & TXN_VIEW_CROP) {
            txn_apply_crop(tv, txn->ui);
        }
        if (tv->apply & TXN_VIEW_DEST_SIZE) {
            txn_apply_dest_size(tv, txn->ui);
        }
        if (tv->apply & TXN_VIEW_POS) {
            txn_apply_pos(tv, txn->ui);
        }

        if (tv->needs_commit) {
            wl_surface_commit(tv->view->surface->remote);
        }
    }

    wl_surface_commit(txn->ui->tree.surface);
    destroy_txn(txn);
}

struct server_ui *
server_ui_create(struct server *server, struct config *cfg) {
    struct server_ui *ui = zalloc(1, sizeof(*ui));

    ui->server = server;

    ui->empty_region = wl_compositor_create_region(server->backend->compositor);
    check_alloc(ui->empty_region);

    ui->root.surface = wl_compositor_create_surface(server->backend->compositor);
    check_alloc(ui->root.surface);

    ui->root.viewport = wp_viewporter_get_viewport(server->backend->viewporter, ui->root.surface);
    check_alloc(ui->root.viewport);

    ui->tree.buffer =
        remote_buffer_manager_color(server->remote_buf, (const uint8_t[4]){0, 0, 0, 0});
    ww_assert(ui->tree.buffer);

    ui->tree.surface = wl_compositor_create_surface(server->backend->compositor);
    check_alloc(ui->tree.surface);
    wl_surface_attach(ui->tree.surface, ui->tree.buffer, 0, 0);
    wl_surface_set_input_region(ui->tree.surface, ui->empty_region);
    wl_surface_commit(ui->tree.surface);

    ui->tree.subsurface = wl_subcompositor_get_subsurface(server->backend->subcompositor,
                                                          ui->tree.surface, ui->root.surface);
    check_alloc(ui->tree.surface);

    ui->xdg_surface = xdg_wm_base_get_xdg_surface(server->backend->xdg_wm_base, ui->root.surface);
    check_alloc(ui->xdg_surface);
    xdg_surface_add_listener(ui->xdg_surface, &xdg_surface_listener, ui);

    ui->xdg_toplevel = xdg_surface_get_toplevel(ui->xdg_surface);
    check_alloc(ui->xdg_toplevel);
    xdg_toplevel_add_listener(ui->xdg_toplevel, &xdg_toplevel_listener, ui);

    if (server->backend->xdg_decoration_manager) {
        ui->xdg_decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
            server->backend->xdg_decoration_manager, ui->xdg_toplevel);
        check_alloc(ui->xdg_decoration);

        zxdg_toplevel_decoration_v1_set_mode(ui->xdg_decoration,
                                             ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }

    wl_list_init(&ui->views);

    wl_signal_init(&ui->events.close);
    wl_signal_init(&ui->events.resize);
    wl_signal_init(&ui->events.view_create);
    wl_signal_init(&ui->events.view_destroy);

    struct server_ui_config *config = server_ui_config_create(ui, cfg);
    if (!config) {
        ww_log(LOG_ERROR, "failed to create server ui config");
        goto fail_config;
    }
    server_ui_use_config(ui, config);

    return ui;

fail_config:
    xdg_toplevel_destroy(ui->xdg_toplevel);
    xdg_surface_destroy(ui->xdg_surface);
    wl_subsurface_destroy(ui->tree.subsurface);
    wl_surface_destroy(ui->tree.surface);
    remote_buffer_deref(ui->tree.buffer);
    wp_viewport_destroy(ui->root.viewport);
    wl_surface_destroy(ui->root.surface);
    wl_region_destroy(ui->empty_region);
    free(ui);
    return NULL;
}

void
server_ui_destroy(struct server_ui *ui) {
    server_ui_config_destroy(ui->config);

    if (ui->xdg_decoration) {
        zxdg_toplevel_decoration_v1_destroy(ui->xdg_decoration);
    }

    xdg_toplevel_destroy(ui->xdg_toplevel);
    xdg_surface_destroy(ui->xdg_surface);
    wl_subsurface_destroy(ui->tree.subsurface);
    wl_surface_destroy(ui->tree.surface);
    remote_buffer_deref(ui->tree.buffer);
    wp_viewport_destroy(ui->root.viewport);
    wl_surface_destroy(ui->root.surface);
    wl_region_destroy(ui->empty_region);

    free(ui);
}

void
server_ui_hide(struct server_ui *ui) {
    ww_assert(ui->mapped);

    wl_surface_attach(ui->root.surface, NULL, 0, 0);
    wl_surface_commit(ui->root.surface);

    ui->mapped = false;
    wl_signal_emit_mutable(&ui->server->events.map_status, &ui->mapped);
}

void
server_ui_show(struct server_ui *ui) {
    ww_assert(!ui->mapped);

    struct wl_display *display = ui->server->backend->display;

    wl_surface_attach(ui->root.surface, NULL, 0, 0);
    wl_surface_commit(ui->root.surface);
    wl_display_roundtrip(display);

    wl_surface_attach(ui->root.surface, ui->config->background, 0, 0);
    wl_surface_commit(ui->root.surface);
    wl_display_roundtrip(display);

    xdg_toplevel_set_title(ui->xdg_toplevel, "waywall");
    xdg_toplevel_set_app_id(ui->xdg_toplevel, "waywall");

    ui->mapped = true;
    wl_signal_emit_mutable(&ui->server->events.map_status, &ui->mapped);
}

void
server_ui_use_config(struct server_ui *ui, struct server_ui_config *config) {
    if (ui->config) {
        server_ui_config_destroy(ui->config);
    }
    ui->config = config;

    if (ui->mapped) {
        wl_surface_attach(ui->root.surface, config->background, 0, 0);
        wl_surface_damage_buffer(ui->root.surface, 0, 0, INT32_MAX, INT32_MAX);
        wl_surface_commit(ui->root.surface);
    }
}

struct server_ui_config *
server_ui_config_create(struct server_ui *ui, struct config *cfg) {
    struct server_ui_config *config = zalloc(1, sizeof(*config));

    config->background = remote_buffer_manager_color(ui->server->remote_buf, cfg->theme.background);
    if (!config->background) {
        ww_log(LOG_ERROR, "failed to create root buffer");
        free(config);
        return NULL;
    }

    return config;
}

void
server_ui_config_destroy(struct server_ui_config *config) {
    remote_buffer_deref(config->background);
    free(config);
}

void
server_view_close(struct server_view *view) {
    return view->impl->close(view->impl_data);
}

pid_t
server_view_get_pid(struct server_view *view) {
    return view->impl->get_pid(view->impl_data);
}

char *
server_view_get_title(struct server_view *view) {
    return view->impl->get_title(view->impl_data);
}

struct server_view *
server_view_create(struct server_ui *ui, struct server_surface *surface,
                   const struct server_view_impl *impl, void *impl_data) {
    struct server_view *view = zalloc(1, sizeof(*view));

    view->ui = ui;
    view->surface = surface;

    view->viewport = wp_viewporter_get_viewport(ui->server->backend->viewporter, surface->remote);
    check_alloc(view->viewport);

    view->state.crop = (struct box){-1, -1, -1, -1};

    view->impl = impl;
    view->impl_data = impl_data;

    view->on_surface_commit.notify = on_view_surface_commit;
    wl_signal_add(&view->surface->events.commit, &view->on_surface_commit);

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
    wl_list_remove(&view->on_surface_commit.link);
    wl_list_remove(&view->link);
    free(view);
}

void
server_ui_apply(struct server_ui *ui, struct server_txn *txn) {
    // If there is an inflight transaction, disregard any pending resizes from it and take over.
    if (ui->inflight_txn) {
        finalize_txn(ui->inflight_txn);
    }
    ui->inflight_txn = txn;

    wl_subsurface_set_sync(ui->tree.subsurface);

    ww_assert(!txn->applied);
    txn->applied = true;

    txn->ui = ui;
    txn->timer = wl_event_loop_add_timer(wl_display_get_event_loop(ui->server->display),
                                         on_txn_timeout, txn);
    check_alloc(txn->timer);

    wl_event_source_timer_update(txn->timer, TXN_TIMEOUT_MS);

    struct server_txn_view *tv;
    wl_list_for_each (tv, &txn->views, link) {
        if (tv->apply & TXN_VIEW_CROP) {
            tv->view->state.crop = tv->crop;
        }
        if (tv->apply & TXN_VIEW_POS) {
            tv->view->state.x = tv->x;
            tv->view->state.y = tv->y;
        }
        if (tv->apply & TXN_VIEW_SIZE) {
            tv->resize_dep.listener.notify = on_txn_view_commit;
            wl_signal_add(&tv->view->surface->events.commit, &tv->resize_dep.listener);

            wl_list_insert(&txn->dependencies, &tv->resize_dep.link);

            tv->view->impl->set_size(tv->view->impl_data, tv->width, tv->height);
        }
        if (tv->apply & TXN_VIEW_VISIBLE) {
            txn_apply_visible(tv, ui);
        }
    }

    if (wl_list_empty(&txn->dependencies)) {
        finalize_txn(txn);
    }
}

struct server_txn *
server_txn_create() {
    struct server_txn *txn = zalloc(1, sizeof(*txn));

    wl_list_init(&txn->views);
    wl_list_init(&txn->dependencies);

    return txn;
}

struct server_txn_view *
server_txn_get_view(struct server_txn *txn, struct server_view *view) {
    struct server_txn_view *txn_view = zalloc(1, sizeof(*txn_view));

    txn_view->parent = txn;
    txn_view->view = view;
    txn_view->needs_commit = true;

    wl_list_insert(&txn->views, &txn_view->link);

    return txn_view;
}

void
server_txn_view_set_above(struct server_txn_view *view, struct wl_surface *surface) {
    view->above = surface;
    view->apply |= TXN_VIEW_ABOVE;
}

void
server_txn_view_set_crop(struct server_txn_view *view, int32_t x, int32_t y, int32_t width,
                         int32_t height) {
    view->crop.x = x;
    view->crop.y = y;
    view->crop.width = width;
    view->crop.height = height;
    view->apply |= TXN_VIEW_CROP;
}

void
server_txn_view_set_dest_size(struct server_txn_view *view, uint32_t width, uint32_t height) {
    view->dest_width = width;
    view->dest_height = height;
    view->apply |= TXN_VIEW_DEST_SIZE;
}

void
server_txn_view_set_pos(struct server_txn_view *view, uint32_t x, uint32_t y) {
    view->x = x;
    view->y = y;
    view->apply |= TXN_VIEW_POS;
}

void
server_txn_view_set_size(struct server_txn_view *view, uint32_t width, uint32_t height) {
    view->width = width;
    view->height = height;
    view->apply |= TXN_VIEW_SIZE;
}

void
server_txn_view_set_visible(struct server_txn_view *view, bool visible) {
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
        rect->subsurface =
            wl_subcompositor_get_subsurface(rect->parent->server->backend->subcompositor,
                                            rect->surface, rect->parent->tree.surface);
        check_alloc(rect->subsurface);

        wl_subsurface_set_position(rect->subsurface, rect->x, rect->y);
        wl_surface_commit(rect->surface);
        wl_surface_commit(rect->parent->tree.surface);
    } else {
        wl_subsurface_destroy(rect->subsurface);
        wl_surface_commit(rect->surface);
        rect->subsurface = NULL;
    }
}
