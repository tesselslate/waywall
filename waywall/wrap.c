#include "wrap.h"
#include "config/action.h"
#include "config/api.h"
#include "config/config.h"
#include "server/cursor.h"
#include "server/server.h"
#include "server/ui.h"
#include "server/wl_seat.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

static void
layout(struct wrap *wrap) {
    if (!wrap->view) {
        return;
    }

    struct transaction *txn = transaction_create();
    ww_assert(txn);

    struct transaction_view *view = transaction_get_view(txn, wrap->view);
    ww_assert(view);

    transaction_view_set_visible(view, true);

    if (wrap->active_res.w == 0) {
        ww_assert(wrap->active_res.h == 0);

        transaction_view_set_behavior(view, TXN_BEHAVIOR_ASYNC);
        transaction_view_set_position(view, 0, 0);
        transaction_view_set_dest_size(view, wrap->width, wrap->height);
        transaction_view_set_size(view, wrap->width, wrap->height);
        transaction_view_set_crop(view, -1, -1, -1, -1);
    } else {
        int32_t x = (wrap->width / 2) - (wrap->active_res.w / 2);
        int32_t y = (wrap->height / 2) - (wrap->active_res.h / 2);

        if (x >= 0 && y >= 0) {
            transaction_view_set_position(view, x, y);
            transaction_view_set_dest_size(view, wrap->active_res.w, wrap->active_res.h);
            transaction_view_set_size(view, wrap->active_res.w, wrap->active_res.h);
            transaction_view_set_crop(view, -1, -1, -1, -1);
        } else {
            // Negative X or Y coordinates mean that the provided resolution is greater than the
            // size of the waywrap window. In this case, we need to crop the view.
            int32_t w = (x >= 0) ? wrap->active_res.w : wrap->width;
            int32_t h = (y >= 0) ? wrap->active_res.h : wrap->height;

            int32_t crop_x = (wrap->active_res.w / 2) - (w / 2);
            int32_t crop_y = (wrap->active_res.h / 2) - (h / 2);

            x = x >= 0 ? x : 0;
            y = y >= 0 ? y : 0;

            transaction_view_set_position(view, x, y);
            transaction_view_set_dest_size(view, w, h);
            transaction_view_set_size(view, wrap->active_res.w, wrap->active_res.h);
            transaction_view_set_crop(view, crop_x, crop_y, w, h);
        }
    }

    transaction_apply(wrap->server->ui, txn);
}

static void
on_close(struct wl_listener *listener, void *data) {
    struct wrap *wrap = wl_container_of(listener, wrap, on_close);

    ww_log(LOG_INFO, "window closed, shutting down");
    server_shutdown(wrap->server);
}

static void
on_pointer_lock(struct wl_listener *listener, void *data) {
    struct wrap *wrap = wl_container_of(listener, wrap, on_pointer_lock);
    server_cursor_hide(wrap->server->cursor);
    wrap->input.pointer_locked = true;

    server_set_pointer_pos(wrap->server, wrap->width / 2.0, wrap->height / 2.0);
}

static void
on_pointer_unlock(struct wl_listener *listener, void *data) {
    struct wrap *wrap = wl_container_of(listener, wrap, on_pointer_unlock);
    server_cursor_show(wrap->server->cursor);
    wrap->input.pointer_locked = false;
}

static void
on_resize(struct wl_listener *listener, void *data) {
    struct wrap *wrap = wl_container_of(listener, wrap, on_resize);

    int32_t new_width = wrap->server->ui->width;
    int32_t new_height = wrap->server->ui->height;

    if (new_width == wrap->width && new_height == wrap->height) {
        return;
    }

    wrap->width = new_width;
    wrap->height = new_height;

    layout(wrap);

    if (wrap->input.pointer_locked) {
        server_set_pointer_pos(wrap->server, wrap->width / 2.0, wrap->height / 2.0);
    }
}

static void
on_view_create(struct wl_listener *listener, void *data) {
    struct wrap *wrap = wl_container_of(listener, wrap, on_view_create);
    struct server_view *view = data;

    if (wrap->view) {
        ww_log(LOG_WARN, "extra toplevel created");
        return;
    }

    wrap->view = view;

    server_set_input_focus(wrap->server, wrap->view);
    layout(wrap);
}

static void
on_view_destroy(struct wl_listener *listener, void *data) {
    struct wrap *wrap = wl_container_of(listener, wrap, on_view_destroy);
    struct server_view *view = data;

    if (wrap->view != view) {
        return;
    }

    wrap->view = NULL;
    server_ui_hide(wrap->server->ui);
    server_shutdown(wrap->server);
}

static bool
on_button(void *data, uint32_t button, bool pressed) {
    return false;
}

static bool
on_key(void *data, xkb_keysym_t sym, bool pressed) {
    struct wrap *wrap = data;

    if (pressed) {
        struct config_action action = {0};
        action.type = CONFIG_ACTION_KEY;
        action.data = sym;
        action.modifiers = wrap->input.modifiers;

        return (config_action_try(wrap->cfg, action) != 0);
    } else {
        return false;
    }
}

static void
on_modifiers(void *data, uint32_t mods) {
    struct wrap *wrap = data;

    wrap->input.modifiers = mods;
}

static void
on_motion(void *data, double x, double y) {
    // Unused.
}

static const struct server_seat_listener seat_listener = {
    .button = on_button,
    .key = on_key,
    .modifiers = on_modifiers,
    .motion = on_motion,
};

struct wrap *
wrap_create(struct server *server, struct config *cfg) {
    struct wrap *wrap = zalloc(1, sizeof(*wrap));

    wrap->cfg = cfg;
    wrap->server = server;

    config_api_set_wrap(wrap->cfg, wrap);

    wrap->on_close.notify = on_close;
    wl_signal_add(&server->ui->events.close, &wrap->on_close);

    wrap->on_pointer_lock.notify = on_pointer_lock;
    wl_signal_add(&server->events.pointer_lock, &wrap->on_pointer_lock);

    wrap->on_pointer_unlock.notify = on_pointer_unlock;
    wl_signal_add(&server->events.pointer_unlock, &wrap->on_pointer_unlock);

    wrap->on_resize.notify = on_resize;
    wl_signal_add(&server->ui->events.resize, &wrap->on_resize);

    wrap->on_view_create.notify = on_view_create;
    wl_signal_add(&server->ui->events.view_create, &wrap->on_view_create);

    wrap->on_view_destroy.notify = on_view_destroy;
    wl_signal_add(&server->ui->events.view_destroy, &wrap->on_view_destroy);

    server_seat_set_listener(server->seat, &seat_listener, wrap);

    return wrap;
}

void
wrap_destroy(struct wrap *wrap) {
    wl_list_remove(&wrap->on_close.link);
    wl_list_remove(&wrap->on_pointer_lock.link);
    wl_list_remove(&wrap->on_pointer_unlock.link);
    wl_list_remove(&wrap->on_resize.link);
    wl_list_remove(&wrap->on_view_create.link);
    wl_list_remove(&wrap->on_view_destroy.link);

    free(wrap);
}

int
wrap_set_config(struct wrap *wrap, struct config *cfg) {
    struct server_config *server_config = server_config_create(wrap->server, cfg);
    if (!server_config) {
        ww_log(LOG_ERROR, "failed to create server config");
        return 1;
    }

    server_use_config(wrap->server, server_config);
    server_config_destroy(server_config);

    config_api_set_wrap(cfg, wrap);

    wrap->cfg = cfg;
    return 0;
}

int
wrap_lua_set_res(struct wrap *wrap, int32_t width, int32_t height) {
    if ((width == 0) != (height == 0)) {
        return 1;
    }

    wrap->active_res.w = width;
    wrap->active_res.h = height;
    layout(wrap);

    return 0;
}
