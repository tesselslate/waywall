#include "server/ui.h"
#include "alpha-modifier-v1-client-protocol.h"
#include "config/config.h"
#include "server/backend.h"
#include "server/buffer.h"
#include "server/server.h"
#include "server/wl_compositor.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "tearing-control-v1-client-protocol.h"
#include "util/alloc.h"
#include "util/debug.h"
#include "util/log.h"
#include "util/png.h"
#include "util/prelude.h"
#include "util/syscall.h"
#include "viewporter-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <linux/memfd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

static constexpr int32_t DEFAULT_WIDTH = 640;
static constexpr int32_t DEFAULT_HEIGHT = 480;

#define COLOR_BLEND(c, a) (COLOR_MULT(((c) * (a)) / UINT8_MAX))
#define COLOR_MULT(c) ((uint32_t)(((uint64_t)(c) * UINT32_MAX) / UINT8_MAX))

struct bg_buffer {
    int fd;
};

static char *
bg_buffer_alloc(struct bg_buffer *buffer, size_t size) {
    buffer->fd = memfd_create("waywall-shm", MFD_CLOEXEC);
    if (buffer->fd == -1) {
        ww_log_errno(LOG_ERROR, "failed to create memfd for background buffer");
        return nullptr;
    }

    if (ftruncate(buffer->fd, size) == -1) {
        ww_log_errno(LOG_ERROR, "failed to truncate memfd for background buffer");
        goto fail;
    }

    char *buf = mmap(nullptr, size, PROT_WRITE, MAP_SHARED, buffer->fd, 0);
    if (buf == MAP_FAILED) {
        ww_log_errno(LOG_ERROR, "failed to mmap memfd for background buffer");
        goto fail;
    }

    return buf;

fail:
    close(buffer->fd);
    buffer->fd = -1;
    return nullptr;
}

static struct wl_buffer *
bg_buffer_create(struct server *server, struct bg_buffer *data, int32_t width, int32_t height) {
    size_t size = (size_t)width * (size_t)height * 4;

    struct wl_shm_pool *pool = wl_shm_create_pool(server->backend->shm, data->fd, size);
    struct wl_buffer *buffer =
        wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888);
    wl_buffer_set_user_data(buffer, data);
    wl_shm_pool_destroy(pool);

    return buffer;
}

static void
bg_buffer_destroy(struct wl_buffer *buffer) {
    struct bg_buffer *data = wl_buffer_get_user_data(buffer);
    wl_buffer_destroy(buffer);

    if (!data) {
        return;
    }

    if (close(data->fd) != 0) {
        ww_log_errno(LOG_ERROR, "failed to close memfd for background buffer");
    }

    free(data);
}

static struct wl_buffer *
color_buffer_new(struct server *server, const uint8_t color[static 4]) {
    if (server->backend->single_pixel_buffer_manager) {
        return wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
            server->backend->single_pixel_buffer_manager, COLOR_BLEND(color[0], color[3]),
            COLOR_BLEND(color[1], color[3]), COLOR_BLEND(color[2], color[3]), COLOR_MULT(color[3]));
    }

    struct bg_buffer *data = zalloc(1, sizeof(*data));
    char *buf = bg_buffer_alloc(data, 4);
    if (!buf) {
        free(data);
        return nullptr;
    }

    // Reorder bytes to ARGB and do alpha premultiplication.
    buf[0] = (uint32_t)(color[2] * color[3]) / UINT8_MAX;
    buf[1] = (uint32_t)(color[1] * color[3]) / UINT8_MAX;
    buf[2] = (uint32_t)(color[0] * color[3]) / UINT8_MAX;
    buf[3] = color[3];

    return bg_buffer_create(server, data, 1, 1);
}

static struct wl_buffer *
image_buffer_new(struct server *server, const char *path) {
    struct util_png png = util_png_decode(path, 16384); // arbitrary max size
    if (!png.data) {
        return nullptr;
    }

    struct bg_buffer *data = zalloc(1, sizeof(*data));
    char *buf = bg_buffer_alloc(data, png.size);
    if (!buf) {
        free(png.data);
        free(data);
        return nullptr;
    }

    size_t pixels = png.width * png.height;
    for (size_t i = 0; i < pixels; i++) {
        buf[i * 4 + 0] = png.data[i * 4 + 2];
        buf[i * 4 + 1] = png.data[i * 4 + 1];
        buf[i * 4 + 2] = png.data[i * 4 + 0];
        buf[i * 4 + 3] = png.data[i * 4 + 3];
    }

    free(png.data);

    return bg_buffer_create(server, data, png.width, png.height);
}

static void
layout_centered(struct server_view *view) {
    ww_assert(view->subsurface);

    int32_t width, height;
    server_buffer_get_size(server_surface_next_buffer(view->surface), &width, &height);

    // Center the view in the window.
    int32_t x = (view->ui->width / 2) - (width / 2);
    int32_t y = (view->ui->height / 2) - (height / 2);

    if (x >= 0 && y >= 0) {
        // If the centered view is entirely inside the window, it can be shown as normal.
        wl_subsurface_set_position(view->subsurface, x, y);
        wp_viewport_set_source(view->viewport, wl_fixed_from_int(-1), wl_fixed_from_int(-1),
                               wl_fixed_from_int(-1), wl_fixed_from_int(-1));
        wp_viewport_set_destination(view->viewport, -1, -1);
    } else {
        // If the centered view is partially outside the window, it must be cropped.
        int32_t crop_width = (x >= 0) ? width : view->ui->width;
        int32_t crop_height = (y >= 0) ? height : view->ui->height;

        int32_t crop_x = (width / 2) - (crop_width / 2);
        int32_t crop_y = (height / 2) - (crop_height / 2);

        x = x >= 0 ? x : 0;
        y = y >= 0 ? y : 0;

        wl_subsurface_set_position(view->subsurface, x, y);
        wp_viewport_set_source(view->viewport, wl_fixed_from_int(crop_x), wl_fixed_from_int(crop_y),
                               wl_fixed_from_int(crop_width), wl_fixed_from_int(crop_height));
        wp_viewport_set_destination(view->viewport, crop_width, crop_height);
    }

    view->current.x = x;
    view->current.y = y;
    wl_surface_commit(view->ui->tree.surface);
}

static void
layout_floating(struct server_view *view) {
    ww_assert(view->subsurface);

    wl_subsurface_set_position(view->subsurface, view->current.x, view->current.y);

    // HACK: This is to work around a bug in Smithay
    // (https://github.com/Smithay/smithay/issues/1894). This can be made better once I rework
    // surface state.
    wl_subsurface_set_sync(view->subsurface);

    wp_viewport_set_source(view->viewport, wl_fixed_from_int(-1), wl_fixed_from_int(-1),
                           wl_fixed_from_int(-1), wl_fixed_from_int(-1));
    wp_viewport_set_destination(view->viewport, -1, -1);

    wl_surface_commit(view->ui->tree.surface);
}

static void
view_state_reset(struct server_view_state *state) {
    *state = (struct server_view_state){};
}

static void
on_xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel) {
    struct server_ui *ui = data;

    if (!ui->mapped) {
        ww_log(LOG_WARN, "received spurious xdg_toplevel.close from remote compositor");
        return;
    }

    wl_signal_emit_mutable(&ui->events.close, nullptr);
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

    bool fullscreen = false;

    uint32_t *state;
    wl_array_for_each(state, states) {
        if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN) {
            fullscreen = true;
        }
    }

    ui->fullscreen = fullscreen;
    WW_DEBUG(ui.fullscreen, fullscreen);
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
        wl_signal_emit_mutable(&ui->events.resize, nullptr);
        ui->resize = false;

        WW_DEBUG(ui.width, ui->width);
        WW_DEBUG(ui.height, ui->height);
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

    if (view->surface->current.buffer) {
        int32_t prev_width, prev_height;
        int32_t pending_width, pending_height;

        server_buffer_get_size(view->surface->current.buffer, &prev_width, &prev_height);
        server_buffer_get_size(view->surface->pending.buffer, &pending_width, &pending_height);

        if (prev_width != pending_width || prev_height != pending_height) {
            wl_signal_emit_mutable(&view->events.resize, nullptr);
        }
    }

    if (view->subsurface && view->current.centered) {
        layout_centered(view);
    }
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

    if (server->backend->tearing_control) {
        ui->root.tearing_control = wp_tearing_control_manager_v1_get_tearing_control(
            server->backend->tearing_control, ui->root.surface);
        check_alloc(ui->root.tearing_control);
    }

    ui->tree.buffer = color_buffer_new(server, (const uint8_t[4]){0, 0, 0, 0});
    ww_assert(ui->tree.buffer);

    ui->tree.surface = wl_compositor_create_surface(server->backend->compositor);
    check_alloc(ui->tree.surface);
    wl_surface_attach(ui->tree.surface, ui->tree.buffer, 0, 0);
    wl_surface_set_input_region(ui->tree.surface, ui->empty_region);
    wl_surface_commit(ui->tree.surface);

    ui->tree.subsurface = wl_subcompositor_get_subsurface(server->backend->subcompositor,
                                                          ui->tree.surface, ui->root.surface);
    check_alloc(ui->tree.surface);

    wl_subsurface_set_desync(ui->tree.subsurface);

    ui->xdg_surface = xdg_wm_base_get_xdg_surface(server->backend->xdg_wm_base, ui->root.surface);
    check_alloc(ui->xdg_surface);
    xdg_surface_add_listener(ui->xdg_surface, &xdg_surface_listener, ui);

    ui->xdg_toplevel = xdg_surface_get_toplevel(ui->xdg_surface);
    check_alloc(ui->xdg_toplevel);
    xdg_toplevel_add_listener(ui->xdg_toplevel, &xdg_toplevel_listener, ui);

    xdg_toplevel_set_title(ui->xdg_toplevel, "waywall");
    xdg_toplevel_set_app_id(ui->xdg_toplevel, "waywall");

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
    if (ui->root.tearing_control) {
        wp_tearing_control_v1_destroy(ui->root.tearing_control);
    }
    if (ui->xdg_decoration) {
        zxdg_toplevel_decoration_v1_destroy(ui->xdg_decoration);
    }

    xdg_toplevel_destroy(ui->xdg_toplevel);
    xdg_surface_destroy(ui->xdg_surface);
    wl_subsurface_destroy(ui->tree.subsurface);
    wl_surface_destroy(ui->tree.surface);
    bg_buffer_destroy(ui->tree.buffer);
    wp_viewport_destroy(ui->root.viewport);
    wl_surface_destroy(ui->root.surface);
    wl_region_destroy(ui->empty_region);
    free(ui);
    return nullptr;
}

void
server_ui_destroy(struct server_ui *ui) {
    server_ui_config_destroy(ui->config);

    if (ui->root.tearing_control) {
        wp_tearing_control_v1_destroy(ui->root.tearing_control);
    }
    if (ui->xdg_decoration) {
        zxdg_toplevel_decoration_v1_destroy(ui->xdg_decoration);
    }

    xdg_toplevel_destroy(ui->xdg_toplevel);
    xdg_surface_destroy(ui->xdg_surface);
    wl_subsurface_destroy(ui->tree.subsurface);
    wl_surface_destroy(ui->tree.surface);
    bg_buffer_destroy(ui->tree.buffer);
    wp_viewport_destroy(ui->root.viewport);
    wl_surface_destroy(ui->root.surface);
    wl_region_destroy(ui->empty_region);

    free(ui);
}

void
server_ui_hide(struct server_ui *ui) {
    ww_assert(ui->mapped);

    wl_surface_attach(ui->root.surface, nullptr, 0, 0);
    wl_surface_commit(ui->root.surface);

    ui->mapped = false;
    wl_signal_emit_mutable(&ui->server->events.map_status, &ui->mapped);
}

void
server_ui_set_fullscreen(struct server_ui *ui, bool fullscreen) {
    if (ui->fullscreen == fullscreen) {
        return;
    }

    if (fullscreen) {
        xdg_toplevel_set_fullscreen(ui->xdg_toplevel, nullptr);
    } else {
        xdg_toplevel_unset_fullscreen(ui->xdg_toplevel);
    }

    ui->fullscreen = fullscreen;
    WW_DEBUG(ui.fullscreen, fullscreen);
}

void
server_ui_show(struct server_ui *ui) {
    ww_assert(!ui->mapped);

    struct wl_display *display = ui->server->backend->display;

    wl_surface_attach(ui->root.surface, nullptr, 0, 0);
    wl_surface_commit(ui->root.surface);
    wl_display_roundtrip(display);

    wl_surface_attach(ui->root.surface, ui->config->background, 0, 0);
    wl_surface_commit(ui->root.surface);
    wl_display_roundtrip(display);

    ui->mapped = true;
    wl_signal_emit_mutable(&ui->server->events.map_status, &ui->mapped);
}

void
server_ui_use_config(struct server_ui *ui, struct server_ui_config *config) {
    if (ui->config) {
        server_ui_config_destroy(ui->config);
    }
    ui->config = config;

    if (ui->root.tearing_control) {
        wp_tearing_control_v1_set_presentation_hint(
            ui->root.tearing_control, config->tearing
                                          ? WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC
                                          : WP_TEARING_CONTROL_V1_PRESENTATION_HINT_VSYNC);
    }
    if (ui->mapped) {
        wl_surface_attach(ui->root.surface, config->background, 0, 0);
        wl_surface_damage_buffer(ui->root.surface, 0, 0, INT32_MAX, INT32_MAX);
        wl_surface_commit(ui->root.surface);
    }

    struct server_view *view;
    wl_list_for_each (view, &ui->views, link) {
        if (!view->current.centered && view->alpha_surface) {
            wp_alpha_modifier_surface_v1_set_multiplier(view->alpha_surface, config->ninb_opacity);
        }
    }
}

struct server_ui_config *
server_ui_config_create(struct server_ui *ui, struct config *cfg) {
    struct server_ui_config *config = zalloc(1, sizeof(*config));

    if (*cfg->theme.background_path) {
        config->background = image_buffer_new(ui->server, cfg->theme.background_path);
    } else {
        config->background = color_buffer_new(ui->server, cfg->theme.background);
    }

    if (!config->background) {
        ww_log(LOG_ERROR, "failed to create background buffer");
        goto fail;
    }

    config->tearing = cfg->experimental.tearing;

    config->ninb_opacity = cfg->theme.ninb_opacity * UINT32_MAX;

    return config;

fail:
    free(config);
    return nullptr;
}

void
server_ui_config_destroy(struct server_ui_config *config) {
    bg_buffer_destroy(config->background);
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

void
server_view_commit(struct server_view *view) {
    bool size_changed = false;
    bool visibility_changed = false;

    if (view->pending.present & VIEW_STATE_CENTERED) {
        view->current.centered = view->pending.centered;

        if (view->alpha_surface) {
            wp_alpha_modifier_surface_v1_set_multiplier(
                view->alpha_surface,
                view->current.centered ? UINT32_MAX : view->ui->config->ninb_opacity);
        }
    }
    if (view->pending.present & VIEW_STATE_POS) {
        view->current.x = view->pending.x;
        view->current.y = view->pending.y;
    }
    if (view->pending.present & VIEW_STATE_SIZE) {
        size_changed = view->current.width != view->pending.width ||
                       view->current.height != view->pending.height;

        view->current.width = view->pending.width;
        view->current.height = view->pending.height;
    }
    if (view->pending.present & VIEW_STATE_VISIBLE) {
        visibility_changed = view->current.visible != view->pending.visible;

        view->current.visible = view->pending.visible;
    }

    if (size_changed) {
        view->impl->set_size(view->impl_data, view->current.width, view->current.height);
    }

    if (visibility_changed && view->current.visible) {
        ww_assert(!view->subsurface);

        view->subsurface =
            wl_subcompositor_get_subsurface(view->ui->server->backend->subcompositor,
                                            view->surface->remote, view->ui->tree.surface);
        check_alloc(view->subsurface);

        wl_subsurface_set_desync(view->subsurface);
    } else if (visibility_changed && !view->current.visible) {
        ww_assert(view->subsurface);

        wl_subsurface_destroy(view->subsurface);
        view->subsurface = nullptr;
    }

    if (view->subsurface) {
        if (view->current.centered) {
            layout_centered(view);
        } else {
            layout_floating(view);
        }
    }

    view_state_reset(&view->pending);
}

void
server_view_refresh(struct server_view *view) {
    if (view->subsurface && view->current.centered) {
        layout_centered(view);
    }
}

void
server_view_set_centered(struct server_view *view, bool centered) {
    view->pending.centered = centered;
    view->pending.present |= VIEW_STATE_CENTERED;
}

void
server_view_set_pos(struct server_view *view, uint32_t x, uint32_t y) {
    view->pending.x = x;
    view->pending.y = y;
    view->pending.present |= VIEW_STATE_POS;
}

void
server_view_set_size(struct server_view *view, uint32_t width, uint32_t height) {
    view->pending.width = width;
    view->pending.height = height;
    view->pending.present |= VIEW_STATE_SIZE;
}

void
server_view_set_visible(struct server_view *view, bool visible) {
    view->pending.visible = visible;
    view->pending.present |= VIEW_STATE_VISIBLE;
}

struct server_view *
server_view_create(struct server_ui *ui, struct server_surface *surface,
                   const struct server_view_impl *impl, void *impl_data) {
    struct server_view *view = zalloc(1, sizeof(*view));

    view->ui = ui;
    view->surface = surface;

    view->viewport = wp_viewporter_get_viewport(ui->server->backend->viewporter, surface->remote);
    check_alloc(view->viewport);

    if (ui->server->backend->alpha_modifier) {
        view->alpha_surface = wp_alpha_modifier_v1_get_surface(ui->server->backend->alpha_modifier,
                                                               view->surface->remote);
        check_alloc(view->alpha_surface);
    }

    view->impl = impl;
    view->impl_data = impl_data;

    view->on_surface_commit.notify = on_view_surface_commit;
    wl_signal_add(&view->surface->events.commit, &view->on_surface_commit);

    wl_list_insert(&ui->views, &view->link);

    wl_signal_init(&view->events.destroy);
    wl_signal_init(&view->events.resize);

    wl_signal_emit_mutable(&ui->events.view_create, view);

    return view;
}

void
server_view_destroy(struct server_view *view) {
    wl_signal_emit_mutable(&view->events.destroy, nullptr);
    wl_signal_emit_mutable(&view->ui->events.view_destroy, view);

    if (view->alpha_surface) {
        wp_alpha_modifier_surface_v1_destroy(view->alpha_surface);
    }

    if (view->subsurface) {
        wl_subsurface_destroy(view->subsurface);
    }
    wp_viewport_destroy(view->viewport);
    wl_list_remove(&view->on_surface_commit.link);
    wl_list_remove(&view->link);
    free(view);
}
