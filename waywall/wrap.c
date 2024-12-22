#include "wrap.h"
#include "config/config.h"
#include "config/vm.h"
#include "inotify.h"
#include "instance.h"
#include "scene.h"
#include "server/backend.h"
#include "server/buffer.h"
#include "server/cursor.h"
#include "server/fake_input.h"
#include "server/gl.h"
#include "server/server.h"
#include "server/ui.h"
#include "server/wl_compositor.h"
#include "server/wl_seat.h"
#include "subproc.h"
#include "timer.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <xkbcommon/xkbcommon.h>

#define IS_ANCHORED(wrap, view) (wrap->floating.anchored == view)
#define SHOULD_ANCHOR(wrap) (wrap->cfg->theme.ninb_anchor != ANCHOR_NONE)

static void on_anchored_resize(struct wl_listener *listener, void *data);

struct floating_view {
    struct wl_list link; // wrap.floating.views

    struct server_view *view;
    int32_t x, y;
};

static void
set_anchored(struct wrap *wrap, struct floating_view *fview) {
    ww_assert(!wrap->floating.anchored);

    wrap->floating.anchored = fview;
    wrap->floating.on_anchored_resize.notify = on_anchored_resize;
    wl_signal_add(&fview->view->events.resize, &wrap->floating.on_anchored_resize);
}

static void
unset_anchored(struct wrap *wrap) {
    ww_assert(!!wrap->floating.anchored);

    wrap->floating.anchored = NULL;
    wl_list_remove(&wrap->floating.on_anchored_resize.link);
}

static void
floating_find_anchored(struct wrap *wrap) {
    ww_assert(!wrap->floating.anchored);

    if (wl_list_empty(&wrap->floating.views)) {
        return;
    }

    // Floating views are inserted at the front of the list, so the view which was created earliest
    // is at the end.
    struct floating_view *fview;
    wl_list_for_each_reverse (fview, &wrap->floating.views, link) {
        break;
    }
    set_anchored(wrap, fview);
}

static void
floating_set_visible(struct wrap *wrap, bool visible) {
    if (wrap->floating.visible == visible) {
        return;
    }

    wrap->floating.visible = visible;

    struct floating_view *fview;
    wl_list_for_each (fview, &wrap->floating.views, link) {
        server_view_set_visible(fview->view, visible);
        server_view_commit(fview->view);
    }

    if (!visible) {
        // Stop any active grab if floating windows are being hidden.
        if (wrap->floating.grab) {
            wrap->floating.grab = NULL;
        }

        if (!wrap->view) {
            return;
        }

        // Give the Minecraft instance input focus if it does not already have it.
        if (!server_view_has_focus(wrap->view)) {
            server_set_input_focus(wrap->server, wrap->view);
        }
    }
}

static void
floating_update_anchored(struct wrap *wrap) {
    ww_assert(SHOULD_ANCHOR(wrap));

    if (!wrap->floating.anchored) {
        return;
    }

    struct floating_view *fview = wrap->floating.anchored;
    int32_t win_width, win_height;
    server_buffer_get_size(server_surface_next_buffer(fview->view->surface), &win_width,
                           &win_height);

    uint32_t center_x = (wrap->width / 2) - (win_width / 2);
    uint32_t center_y = (wrap->height / 2) - (win_height / 2);

    uint32_t x, y;

    switch (wrap->cfg->theme.ninb_anchor) {
    case ANCHOR_TOPLEFT:
        x = 0;
        y = 0;
        break;
    case ANCHOR_TOP:
        x = center_x;
        y = 0;
        break;
    case ANCHOR_TOPRIGHT:
        x = wrap->width - win_width;
        y = 0;
        break;
    case ANCHOR_LEFT:
        x = 0;
        y = center_y;
        break;
    case ANCHOR_RIGHT:
        x = wrap->width - win_width;
        y = center_y;
        break;
    case ANCHOR_BOTTOMLEFT:
        x = 0;
        y = wrap->height - win_height;
        break;
    case ANCHOR_BOTTOMRIGHT:
        x = wrap->width - win_width;
        y = wrap->height - win_height;
        break;
    case ANCHOR_NONE:
        ww_unreachable();
    }

    fview->x = x;
    fview->y = y;

    server_view_set_pos(fview->view, x, y);
    server_view_commit(fview->view);
}

static struct floating_view *
floating_view_at(struct wrap *wrap, double x, double y) {
    struct floating_view *fview;
    wl_list_for_each (fview, &wrap->floating.views, link) {
        int32_t width, height;
        server_buffer_get_size(server_surface_next_buffer(fview->view->surface), &width, &height);

        struct box area = {
            .x = fview->x,
            .y = fview->y,
            .width = width,
            .height = height,
        };

        if (x >= area.x && y >= area.y && x <= area.x + area.width && y <= area.y + area.height) {
            return fview;
        }
    }

    return NULL;
}

static void
floating_view_create(struct wrap *wrap, struct server_view *view) {
    struct floating_view *fview = zalloc(1, sizeof(*fview));
    fview->view = view;
    wl_list_insert(&wrap->floating.views, &fview->link);

    if (wrap->floating.visible) {
        server_view_set_visible(view, true);
    }

    if (!SHOULD_ANCHOR(wrap) || wl_list_length(&wrap->floating.views) > 1) {
        server_view_set_centered(view, false);
        server_view_set_pos(view, 0, 0);
        server_view_commit(view);
    } else {
        ww_assert(!wrap->floating.anchored);

        set_anchored(wrap, fview);

        server_view_set_centered(view, false);
        floating_update_anchored(wrap);
    }
}

static void
floating_view_destroy(struct wrap *wrap, struct server_view *view) {
    struct floating_view *fview, *tmp;
    wl_list_for_each_safe (fview, tmp, &wrap->floating.views, link) {
        if (fview->view != view) {
            continue;
        }

        // If this view was anchored, a new view will need to be anchored (if one exists.)
        bool anchored = IS_ANCHORED(wrap, fview);

        // If the destroyed view was being interactively moved, then stop the interactive move.
        if (fview == wrap->floating.grab) {
            wrap->floating.grab = NULL;
        }

        // If the destroyed view was focused, then give focus back to the Minecraft instance.
        if (server_view_has_focus(fview->view)) {
            if (wrap->view) {
                server_set_input_focus(wrap->server, wrap->view);
            }
        }

        wl_list_remove(&fview->link);
        free(fview);

        if (anchored) {
            unset_anchored(wrap);
            floating_find_anchored(wrap);
            floating_update_anchored(wrap);
        }
        return;
    }

    ww_panic("could not find floating view");
}

static void
process_state_update(int wd, uint32_t mask, const char *name, void *data) {
    struct wrap *wrap = data;

    instance_state_update(wrap->instance);
    config_vm_signal_event(wrap->cfg->vm, "state");
}

static void
on_anchored_resize(struct wl_listener *listener, void *data) {
    struct wrap_floating *wrap_floating =
        wl_container_of(listener, wrap_floating, on_anchored_resize);
    struct wrap *wrap = wl_container_of(wrap_floating, wrap, floating);

    floating_update_anchored(wrap);
}

static void
on_close(struct wl_listener *listener, void *data) {
    struct wrap *wrap = wl_container_of(listener, wrap, on_close);

    // When the view is destroyed, the server event loop will be stopped and waywall will exit.
    // See `on_view_destroy`.
    server_view_close(wrap->view);
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

    if (wrap->view) {
        if (wrap->active_res.w == 0) {
            server_view_set_size(wrap->view, wrap->width, wrap->height);
            server_view_commit(wrap->view);
        } else {
            server_view_refresh(wrap->view);
        }
    }

    if (SHOULD_ANCHOR(wrap)) {
        floating_update_anchored(wrap);
    }

    if (wrap->input.pointer_locked) {
        server_set_pointer_pos(wrap->server, wrap->width / 2.0, wrap->height / 2.0);
    }
}

static void
on_view_create(struct wl_listener *listener, void *data) {
    struct wrap *wrap = wl_container_of(listener, wrap, on_view_create);
    struct server_view *view = data;

    if (wrap->view) {
        floating_view_create(wrap, view);
        return;
    }

    wrap->view = view;
    wrap->instance = instance_create(view, wrap->inotify);
    if (wrap->instance) {
        str path = instance_get_state_path(wrap->instance);

        wrap->instance->state_wd =
            inotify_subscribe(wrap->inotify, path, IN_MODIFY, process_state_update, wrap);
        if (wrap->instance->state_wd == -1) {
            ww_log(LOG_ERROR, "failed to watch instance state");
            instance_destroy(wrap->instance);
            wrap->instance = NULL;
        }

        str_free(path);
    }

    // HACK: This is not ideal. We know that the xdg_toplevel view is created as a result of the
    // xdg_surface role commit event, so the pending buffer will not have been put into the
    // current state yet. I would like to have a better API for this (perhaps change when the
    // event fires?), but this works for now.
    ww_assert(wrap->view->surface->pending.buffer);

    int32_t width, height;
    server_buffer_get_size(wrap->view->surface->pending.buffer, &width, &height);

    wrap->server->ui->width = width;
    wrap->server->ui->height = height;

    server_set_input_focus(wrap->server, wrap->view);
    server_ui_show(wrap->server->ui);

    ww_assert(wrap->width > 0 && wrap->height > 0);
    ww_assert(wrap->view);

    server_view_set_centered(wrap->view, true);
    server_view_set_visible(wrap->view, true);
    server_view_commit(wrap->view);

    // HACK: This is so that scene objects (images, mirrors, text) appear over the instance. This is
    // probably not the best spot to do it, though.
    wl_subsurface_place_below(wrap->view->subsurface, wrap->view->ui->tree.surface);
    wl_surface_commit(wrap->view->surface->remote);

    server_gl_set_capture(wrap->gl, view->surface);
}

static void
on_view_destroy(struct wl_listener *listener, void *data) {
    struct wrap *wrap = wl_container_of(listener, wrap, on_view_destroy);
    struct server_view *view = data;

    if (wrap->view != view) {
        floating_view_destroy(wrap, view);
        return;
    }

    if (wrap->instance) {
        inotify_unsubscribe(wrap->inotify, wrap->instance->state_wd);
        instance_destroy(wrap->instance);
        wrap->instance = NULL;
    }

    wrap->view = NULL;
    server_ui_hide(wrap->server->ui);
    server_shutdown(wrap->server);
}

static bool
on_button(void *data, uint32_t button, bool pressed) {
    struct wrap *wrap = data;

    // Attempt to match the button press with an action.
    if (pressed) {
        struct config_action action = {0};
        action.type = CONFIG_ACTION_BUTTON;
        action.data = button;
        action.modifiers = wrap->input.modifiers;

        ssize_t idx = config_find_action(wrap->cfg, &action);
        if (idx >= 0) {
            if (config_vm_try_action(wrap->cfg->vm, idx)) {
                return true;
            }
        }
    }

    // Process the active grab, if any.
    if (wrap->floating.grab) {
        if (button == BTN_LEFT && !pressed) {
            wrap->floating.grab = NULL;
        }

        return true;
    }

    // If Minecraft has locked the pointer, just send it the button event. Do not check if it should
    // be sent to a floating window.
    if (wrap->input.pointer_locked) {
        return false;
    }

    // Whether or not a window grab can be initiated by this event.
    // The user must have started pressing the left mouse button while holding shift.
    bool should_grab = (pressed && button == BTN_LEFT && (wrap->input.modifiers & KB_MOD_SHIFT));

    // Check to see if the input focus should be changed to a new window. If the user did not click
    // on any floating window, then input focus should be given back to the Minecraft instance.
    struct floating_view *fview = floating_view_at(wrap, wrap->input.x, wrap->input.y);
    if (!fview) {
        ww_assert(wrap->view);
        server_set_input_focus(wrap->server, wrap->view);
        return false;
    }

    // If a window grab should start, then start it. Otherwise, focus the window that the user
    // clicked. (Only happens when the floating view is visible)
    if (!wrap->floating.visible) {
        return false;
    }

    if (should_grab && !IS_ANCHORED(wrap, fview)) {
        wrap->floating.grab = fview;
        wrap->floating.grab_x = wrap->input.x - fview->x;
        wrap->floating.grab_y = wrap->input.y - fview->y;
        server_set_input_focus(wrap->server, fview->view);
        return true;
    } else {
        server_set_input_focus(wrap->server, fview->view);
        return false;
    }
}

static bool
on_key(void *data, size_t num_syms, const xkb_keysym_t syms[static num_syms], bool pressed) {
    struct wrap *wrap = data;

    if (!pressed) {
        return false;
    }

    struct config_action action = {0};
    action.type = CONFIG_ACTION_KEY;
    action.modifiers = wrap->input.modifiers;

    for (size_t i = 0; i < num_syms; i++) {
        action.data = syms[i];

        ssize_t idx = config_find_action(wrap->cfg, &action);
        if (idx >= 0) {
            if (config_vm_try_action(wrap->cfg->vm, idx)) {
                return true;
            }
        }
    }

    return false;
}

static void
on_modifiers(void *data, uint32_t mods) {
    struct wrap *wrap = data;

    wrap->input.modifiers = mods;
}

static void
on_motion(void *data, double x, double y) {
    struct wrap *wrap = data;

    wrap->input.x = x;
    wrap->input.y = y;

    if (!wrap->floating.grab) {
        return;
    }

    int32_t view_x = x - wrap->floating.grab_x;
    int32_t view_y = y - wrap->floating.grab_y;

    wrap->floating.grab->x = view_x;
    wrap->floating.grab->y = view_y;

    server_view_set_pos(wrap->floating.grab->view, view_x, view_y);
    server_view_commit(wrap->floating.grab->view);
}

static const struct server_seat_listener seat_listener = {
    .button = on_button,
    .key = on_key,
    .modifiers = on_modifiers,
    .motion = on_motion,
};

struct wrap *
wrap_create(struct server *server, struct inotify *inotify, struct config *cfg) {
    struct wrap *wrap = zalloc(1, sizeof(*wrap));

    wrap->gl = server_gl_create(server);
    if (!wrap->gl) {
        ww_log(LOG_ERROR, "failed to initialize OpenGL");
        goto fail_gl;
    }

    wrap->scene = scene_create(cfg, wrap->gl, server->ui);
    if (!wrap->scene) {
        ww_log(LOG_ERROR, "failed to create scene");
        goto fail_scene;
    }

    wrap->cfg = cfg;
    wrap->server = server;
    wrap->inotify = inotify;
    wrap->subproc = subproc_create(server);
    wrap->timer = ww_timer_create(server);

    wl_list_init(&wrap->floating.views);

    config_vm_set_wrap(wrap->cfg->vm, wrap);

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

fail_scene:
    server_gl_destroy(wrap->gl);

fail_gl:
    free(wrap);

    return NULL;
}

void
wrap_destroy(struct wrap *wrap) {
    if (wrap->instance) {
        instance_destroy(wrap->instance);
    }

    scene_destroy(wrap->scene);
    server_gl_destroy(wrap->gl);

    ww_timer_destroy(wrap->timer);
    subproc_destroy(wrap->subproc);

    struct floating_view *fview, *tmp_fview;
    wl_list_for_each_safe (fview, tmp_fview, &wrap->floating.views, link) {
        wl_list_remove(&fview->link);
        free(fview);
    }

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

    config_vm_set_wrap(cfg->vm, wrap);

    wrap->cfg = cfg;
    if (wrap->cfg->theme.ninb_anchor == ANCHOR_NONE) {
        // If anchoring has been disabled, ensure there is no anchored view.
        if (wrap->floating.anchored) {
            unset_anchored(wrap);
        }
    } else {
        // If anchoring is enabled, find a view to anchor (if needed) and reposition it.
        if (!wrap->floating.anchored) {
            floating_find_anchored(wrap);
        }
        floating_update_anchored(wrap);
    }

    return 0;
}

void
wrap_lua_exec(struct wrap *wrap, char *cmd[static 64]) {
    subproc_exec(wrap->subproc, cmd);
}

void
wrap_lua_press_key(struct wrap *wrap, uint32_t keycode) {
    if (!wrap->view) {
        return;
    }

    const struct syn_key keys[] = {
        {keycode, true},
        {keycode, false},
    };

    server_view_send_keys(wrap->view, STATIC_ARRLEN(keys), keys);
}

int
wrap_lua_set_res(struct wrap *wrap, int32_t width, int32_t height) {
    if ((width == 0) != (height == 0)) {
        return 1;
    }

    if (!wrap->view) {
        return 1;
    }

    if (wrap->active_res.w == width && wrap->active_res.h == height) {
        return 0;
    }

    wrap->active_res.w = width;
    wrap->active_res.h = height;

    server_view_set_size(wrap->view, wrap->active_res.w > 0 ? wrap->active_res.w : wrap->width,
                         wrap->active_res.h > 0 ? wrap->active_res.h : wrap->height);
    server_view_commit(wrap->view);

    return 0;
}

void
wrap_lua_show_floating(struct wrap *wrap, bool show) {
    floating_set_visible(wrap, show);
}
