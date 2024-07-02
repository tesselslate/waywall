#include "wall.h"
#include "config/action.h"
#include "config/api.h"
#include "config/config.h"
#include "config/event.h"
#include "config/layout.h"
#include "counter.h"
#include "cpu/cgroup.h"
#include "cpu/cpu.h"
#include "inotify.h"
#include "instance.h"
#include "server/cursor.h"
#include "server/server.h"
#include "server/ui.h"
#include "server/wl_compositor.h"
#include "server/wl_seat.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include "util/str.h"
#include <limits.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <wayland-util.h>
#include <xkbcommon/xkbcommon.h>

static_assert(BTN_JOYSTICK - BTN_MOUSE == STATIC_ARRLEN((struct wall){0}.input.buttons));

#define ON_WALL(w) ((w)->active_instance == -1)

static int
get_hovered(struct wall *wall) {
    if (!wall->layout) {
        return -1;
    }

    for (size_t i = 0; i < wall->layout->num_elements; i++) {
        struct config_layout_element *element = &wall->layout->elements[i];
        if (element->type == LAYOUT_ELEMENT_INSTANCE) {
            bool x = element->x <= wall->input.mx && element->x + element->w >= wall->input.mx;
            bool y = element->y <= wall->input.my && element->y + element->h >= wall->input.my;
            if (x && y) {
                return element->data.instance;
            }
        }
    }

    return -1;
}

static void
layout_active(struct wall *wall) {
    ww_assert(!ON_WALL(wall));

    struct server_txn *txn = server_txn_create();
    ww_assert(txn);

    struct server_txn_view *view =
        server_txn_get_view(txn, wall->instances[wall->active_instance]->view);
    ww_assert(view);

    if (wall->active_res.w == 0) {
        ww_assert(wall->active_res.h == 0);

        server_txn_view_set_pos(view, 0, 0);
        server_txn_view_set_dest_size(view, wall->width, wall->height);
        server_txn_view_set_size(view, wall->width, wall->height);
        server_txn_view_set_crop(view, -1, -1, -1, -1);
    } else {
        int32_t x = (wall->width / 2) - (wall->active_res.w / 2);
        int32_t y = (wall->height / 2) - (wall->active_res.h / 2);

        if (x >= 0 && y >= 0) {
            server_txn_view_set_pos(view, x, y);
            server_txn_view_set_dest_size(view, wall->active_res.w, wall->active_res.h);
            server_txn_view_set_size(view, wall->active_res.w, wall->active_res.h);
            server_txn_view_set_crop(view, -1, -1, -1, -1);
        } else {
            // Negative X or Y coordinates mean that the provided resolution is greater than the
            // size of the waywall window. In this case, we need to crop the view.
            int32_t w = (x >= 0) ? wall->active_res.w : wall->width;
            int32_t h = (y >= 0) ? wall->active_res.h : wall->height;

            int32_t crop_x = (wall->active_res.w / 2) - (w / 2);
            int32_t crop_y = (wall->active_res.h / 2) - (h / 2);

            x = x >= 0 ? x : 0;
            y = y >= 0 ? y : 0;

            server_txn_view_set_pos(view, x, y);
            server_txn_view_set_dest_size(view, w, h);
            server_txn_view_set_size(view, wall->active_res.w, wall->active_res.h);
            server_txn_view_set_crop(view, crop_x, crop_y, w, h);
        }
    }

    server_ui_apply(wall->server->ui, txn);
}

static void
destroy_rectangles(struct wall *wall) {
    for (ssize_t i = 0; i < wall->layout_rectangles.len; i++) {
        ui_rectangle_destroy(wall->layout_rectangles.data[i]);
        wall->layout_rectangles.data[i] = NULL;
    }

    wall->layout_rectangles.len = 0;
}

static void
add_rectangle(struct wall *wall, struct ui_rectangle *rect) {
    if (wall->layout_rectangles.cap == wall->layout_rectangles.len) {
        ww_assert(wall->layout_rectangles.cap < SSIZE_MAX / 2);
        ww_assert(wall->layout_rectangles.cap > 0);

        ssize_t cap = wall->layout_rectangles.cap * 2;
        void *data =
            realloc(wall->layout_rectangles.data, cap * sizeof(*wall->layout_rectangles.data));
        check_alloc(data);

        wall->layout_rectangles.data = data;
        wall->layout_rectangles.cap = cap;
    }

    wall->layout_rectangles.data[wall->layout_rectangles.len++] = rect;
}

static void
layout_wall(struct wall *wall) {
    ww_assert(ON_WALL(wall));

    if (!wall->layout) {
        return;
    }

    struct server_txn *txn = server_txn_create();
    ww_assert(txn);

    destroy_rectangles(wall);

    // Used for Z ordering.
    struct wl_surface *previous = wall->server->ui->tree.surface;

    bool shown[MAX_INSTANCES] = {0};
    for (size_t i = 0; i < wall->layout->num_elements; i++) {
        struct config_layout_element *element = &wall->layout->elements[i];

        bool valid_bounds =
            (element->x + element->w <= wall->width) && (element->y + element->h <= wall->height);

        switch (element->type) {
        case LAYOUT_ELEMENT_INSTANCE:
            ww_assert(element->data.instance >= 0 && element->data.instance < wall->num_instances);

            if (!valid_bounds) {
                ww_log(LOG_WARN, "layout element (instance %d) extends out of window bounds",
                       element->data.instance);
                continue;
            }

            struct server_txn_view *view =
                server_txn_get_view(txn, wall->instances[element->data.instance]->view);
            ww_assert(view);

            server_txn_view_set_dest_size(view, element->w, element->h);
            server_txn_view_set_pos(view, element->x, element->y);
            server_txn_view_set_visible(view, true);
            server_txn_view_set_above(view, previous);
            shown[element->data.instance] = true;

            previous = view->view->surface->remote;
            break;
        case LAYOUT_ELEMENT_RECTANGLE:
            if (!valid_bounds) {
                ww_log(LOG_WARN, "layout element (rectangle) extends out of window bounds");
                continue;
            }

            struct ui_rectangle *rect =
                ui_rectangle_create(wall->server->ui, element->x, element->y, element->w,
                                    element->h, element->data.rectangle);
            ww_assert(rect);

            ui_rectangle_set_visible(rect, true);
            add_rectangle(wall, rect);
            break;
        }
    }

    for (int i = 0; i < wall->num_instances; i++) {
        if (!shown[i]) {
            struct server_txn_view *view = server_txn_get_view(txn, wall->instances[i]->view);
            ww_assert(view);

            server_txn_view_set_visible(view, false);
        }
    }

    server_ui_apply(wall->server->ui, txn);
}

static void
change_layout(struct wall *wall, struct config_layout *layout) {
    if (!layout) {
        return;
    }

    if (wall->layout) {
        config_layout_destroy(wall->layout);
    }

    wall->layout = layout;

    if (ON_WALL(wall)) {
        layout_wall(wall);
    }
}

static void
fixup_layout(struct wall *wall, int id) {
    // This function is called when an instance dies and there is no new layout from the user's
    // layout generator. In this case, we will need to shift instance IDs of any later instances
    // (those with higher IDs) and remove the element containing the instance that died if one
    // exists.
    if (!wall->layout) {
        return;
    }

    size_t w = 0;
    for (size_t i = 0; i < wall->layout->num_elements; i++) {
        struct config_layout_element *elem = &wall->layout->elements[i];

        switch (elem->type) {
        case LAYOUT_ELEMENT_INSTANCE:
            if (elem->data.instance == id) {
                continue;
            } else if (elem->data.instance > id) {
                elem->data.instance--;
                wall->layout->elements[w++] = *elem;
            }
            break;
        default:
            wall->layout->elements[w++] = *elem;
            break;
        }
    }

    wall->layout->num_elements = w;
}

static bool
process_action(struct wall *wall, struct config_action action) {
    bool consumed = (config_action_try(wall->cfg, action) != 0);

    if (consumed) {
        struct config_layout *layout = config_layout_get(wall->cfg, wall);
        change_layout(wall, layout);
    }

    return consumed;
}

static void
process_state_update(int wd, uint32_t mask, const char *name, void *data) {
    struct wall *wall = data;

    int id = -1;
    for (id = 0; id < wall->num_instances; id++) {
        if (wall->instances[id]->state_wd == wd) {
            break;
        }
    }
    if (id == -1) {
        ww_log(LOG_WARN, "unknown watch descriptor %d", wd);
        return;
    }

    int screen = wall->instances[id]->state.screen;
    int percent = (screen == SCREEN_PREVIEWING) ? wall->instances[id]->state.data.percent : -1;

    instance_state_update(wall->instances[id]);
    if (wall->cpu) {
        cpu_update(wall->cpu, id, wall->instances[id]);
    }

    if (screen != SCREEN_PREVIEWING && wall->instances[id]->state.screen == SCREEN_PREVIEWING) {
        config_signal_preview_start(wall->cfg, wall, id);
        struct config_layout *layout = config_layout_get(wall->cfg, wall);
        change_layout(wall, layout);
    } else if (wall->instances[id]->state.screen == SCREEN_PREVIEWING &&
               percent != wall->instances[id]->state.data.percent) {
        config_signal_preview_percent(wall->cfg, wall, id, wall->instances[id]->state.data.percent);
        struct config_layout *layout = config_layout_get(wall->cfg, wall);
        change_layout(wall, layout);
    }
}

static void
add_instance(struct wall *wall, struct instance *instance) {
    str state_path = instance_get_state_path(instance);
    ww_assert(state_path);

    int wd = inotify_subscribe(wall->inotify, state_path, IN_MODIFY, process_state_update, wall);
    if (wd == -1) {
        ww_log(LOG_ERROR, "failed to watch instance state");
        goto fail_wd;
    }
    instance->state_wd = wd;

    int id = wall->num_instances;
    wall->instances[wall->num_instances++] = instance;

    config_signal_spawn(wall->cfg, wall, id);
    struct config_layout *layout = config_layout_get(wall->cfg, wall);
    change_layout(wall, layout);

    if (wall->cpu) {
        cpu_add(wall->cpu, id, instance);
    }

    str_free(state_path);
    return;

fail_wd:
    str_free(state_path);
    instance_destroy(instance);
}

static void
focus_wall(struct wall *wall) {
    ww_assert(!ON_WALL(wall));

    wall->active_instance = -1;
    server_set_input_focus(wall->server, NULL);

    if (wall->cpu) {
        cpu_set_active(wall->cpu, -1);
    }

    layout_wall(wall);
}

static void
remove_instance(struct wall *wall, int id) {
    inotify_unsubscribe(wall->inotify, wall->instances[id]->state_wd);
    instance_destroy(wall->instances[id]);

    if (wall->cpu) {
        if (wall->active_instance == id) {
            cpu_set_active(wall->cpu, -1);
        }
        cpu_remove(wall->cpu, id);
    }

    memmove(&wall->instances[id], &wall->instances[id + 1],
            sizeof(struct instance *) * (wall->num_instances - id - 1));
    wall->num_instances--;

    fixup_layout(wall, id);
    if (wall->active_instance == id) {
        focus_wall(wall);
    }

    config_signal_death(wall->cfg, wall, id);
    struct config_layout *layout = config_layout_get(wall->cfg, wall);
    if (layout) {
        change_layout(wall, layout);
    }
}

static void
play_instance(struct wall *wall, int id) {
    wall->active_instance = id;

    if (wall->cpu) {
        cpu_set_active(wall->cpu, id);
    }

    destroy_rectangles(wall);

    instance_unpause(wall->instances[id]);
    server_set_input_focus(wall->server, wall->instances[id]->view);

    struct server_txn *txn = server_txn_create();
    ww_assert(txn);

    struct server_txn_view *view = server_txn_get_view(txn, wall->instances[id]->view);
    ww_assert(view);

    server_txn_view_set_pos(view, 0, 0);
    server_txn_view_set_dest_size(view, wall->width, wall->height);
    server_txn_view_set_size(view, wall->width, wall->height);
    server_txn_view_set_crop(view, -1, -1, -1, -1);
    server_txn_view_set_visible(view, true);

    for (int i = 0; i < wall->num_instances; i++) {
        if (i != id) {
            view = server_txn_get_view(txn, wall->instances[i]->view);
            ww_assert(view);

            server_txn_view_set_visible(view, false);
        }
    }

    server_ui_apply(wall->server->ui, txn);
}

static void
on_close(struct wl_listener *listener, void *data) {
    struct wall *wall = wl_container_of(listener, wall, on_close);

    // TODO: Allow waywall to keep running in the background
    server_ui_hide(wall->server->ui);
    server_shutdown(wall->server);
}

static void
on_pointer_lock(struct wl_listener *listener, void *data) {
    struct wall *wall = wl_container_of(listener, wall, on_pointer_lock);
    server_cursor_hide(wall->server->cursor);
    wall->input.pointer_locked = true;

    server_set_pointer_pos(wall->server, wall->width / 2.0, wall->height / 2.0);
}

static void
on_pointer_unlock(struct wl_listener *listener, void *data) {
    struct wall *wall = wl_container_of(listener, wall, on_pointer_unlock);
    server_cursor_show(wall->server->cursor);
    wall->input.pointer_locked = false;
}

static void
on_resize(struct wl_listener *listener, void *data) {
    struct wall *wall = wl_container_of(listener, wall, on_resize);

    int32_t new_width = wall->server->ui->width;
    int32_t new_height = wall->server->ui->height;

    if (new_width == wall->width && new_height == wall->height) {
        return;
    }

    wall->width = new_width;
    wall->height = new_height;

    config_signal_resize(wall->cfg, wall, wall->width, wall->height);
    struct config_layout *layout = config_layout_get(wall->cfg, wall);
    change_layout(wall, layout);

    if (!ON_WALL(wall)) {
        layout_active(wall);
    }

    if (wall->input.pointer_locked) {
        server_set_pointer_pos(wall->server, wall->width / 2.0, wall->height / 2.0);
    }
}

static void
on_view_create(struct wl_listener *listener, void *data) {
    struct wall *wall = wl_container_of(listener, wall, on_view_create);
    struct server_view *view = data;

    // TODO: No instance cap
    // Also, I'd like to stop heap allocating all of the instances for cache locality, even
    // though I imagine the bulk of the bad performance will come from libwayland and the server
    // code
    ww_assert(wall->num_instances < MAX_INSTANCES);

    struct instance *instance = instance_create(view, wall->inotify);
    if (!instance) {
        return;
    }

    add_instance(wall, instance);
}

static void
on_view_destroy(struct wl_listener *listener, void *data) {
    struct wall *wall = wl_container_of(listener, wall, on_view_destroy);
    struct server_view *view = data;

    for (int i = 0; i < wall->num_instances; i++) {
        if (wall->instances[i]->view == view) {
            remove_instance(wall, i);
            return;
        }
    }
}

static bool
on_button(void *data, uint32_t button, bool pressed) {
    struct wall *wall = data;

    if (button - BTN_MOUSE >= STATIC_ARRLEN(wall->input.buttons)) {
        return false;
    }

    wall->input.buttons[button - BTN_MOUSE] = pressed;

    if (!ON_WALL(wall)) {
        return false;
    }

    if (pressed) {
        struct config_action action = {0};
        action.type = CONFIG_ACTION_BUTTON;
        action.data = button;
        action.modifiers = wall->input.modifiers;

        wall->last_hovered = get_hovered(wall);
        return process_action(wall, action);
    } else {
        return false;
    }
}

static bool
on_key(void *data, xkb_keysym_t sym, bool pressed) {
    struct wall *wall = data;

    if (pressed) {
        struct config_action action = {0};
        action.type = CONFIG_ACTION_KEY;
        action.data = sym;
        action.modifiers = wall->input.modifiers;

        return process_action(wall, action);
    } else {
        return false;
    }
}

static void
on_modifiers(void *data, uint32_t mods) {
    struct wall *wall = data;

    wall->input.modifiers = mods;
}

static void
on_motion(void *data, double x, double y) {
    struct wall *wall = data;

    wall->input.mx = x;
    wall->input.my = y;

    if (!ON_WALL(wall)) {
        return;
    }

    int hovered = get_hovered(wall);
    if (hovered == wall->last_hovered) {
        return;
    }
    wall->last_hovered = hovered;

    for (size_t i = 0; i < STATIC_ARRLEN(wall->input.buttons); i++) {
        if (wall->input.buttons[i]) {
            struct config_action action = {0};
            action.type = CONFIG_ACTION_BUTTON;
            action.data = i + BTN_MOUSE;
            action.modifiers = wall->input.modifiers;

            process_action(wall, action);
        }
    }
}

static const struct server_seat_listener seat_listener = {
    .button = on_button,
    .key = on_key,
    .modifiers = on_modifiers,
    .motion = on_motion,
};

struct wall *
wall_create(struct server *server, struct inotify *inotify, struct config *cfg) {
    struct wall *wall = zalloc(1, sizeof(*wall));

    wall->cfg = cfg;
    wall->server = server;
    wall->inotify = inotify;

    config_api_set_wall(wall->cfg, wall);

    if (strcmp(cfg->general.counter_path, "") != 0) {
        wall->counter = counter_create(cfg->general.counter_path);
        if (!wall->counter) {
            ww_log(LOG_ERROR, "failed to create reset counter");
            goto fail_counter;
        }
    }

    wall->cpu = cpu_manager_create_cgroup(cfg);
    if (!wall->cpu) {
        ww_log(LOG_ERROR, "failed to create cpu manager");
        goto fail_cpu;
    }

    wall->layout_rectangles.data = zalloc(8, sizeof(*wall->layout_rectangles.data));
    wall->layout_rectangles.len = 0;
    wall->layout_rectangles.cap = 8;

    wall->active_instance = -1;

    wall->on_close.notify = on_close;
    wl_signal_add(&server->ui->events.close, &wall->on_close);

    wall->on_pointer_lock.notify = on_pointer_lock;
    wl_signal_add(&server->events.pointer_lock, &wall->on_pointer_lock);

    wall->on_pointer_unlock.notify = on_pointer_unlock;
    wl_signal_add(&server->events.pointer_unlock, &wall->on_pointer_unlock);

    wall->on_resize.notify = on_resize;
    wl_signal_add(&server->ui->events.resize, &wall->on_resize);

    wall->on_view_create.notify = on_view_create;
    wl_signal_add(&server->ui->events.view_create, &wall->on_view_create);

    wall->on_view_destroy.notify = on_view_destroy;
    wl_signal_add(&server->ui->events.view_destroy, &wall->on_view_destroy);

    server_seat_set_listener(server->seat, &seat_listener, wall);

    return wall;

fail_cpu:
    if (wall->counter) {
        counter_destroy(wall->counter);
    }

fail_counter:
    free(wall);
    return NULL;
}

void
wall_destroy(struct wall *wall) {
    for (int i = 0; i < wall->num_instances; i++) {
        inotify_unsubscribe(wall->inotify, wall->instances[i]->state_wd);
        instance_destroy(wall->instances[i]);
    }
    wall->num_instances = 0;

    if (wall->layout) {
        config_layout_destroy(wall->layout);
    }
    if (wall->layout_rectangles.data) {
        destroy_rectangles(wall);
        free(wall->layout_rectangles.data);
    }

    if (wall->counter) {
        counter_destroy(wall->counter);
    }

    if (wall->cpu) {
        cpu_destroy(wall->cpu);
    }

    wl_list_remove(&wall->on_pointer_lock.link);
    wl_list_remove(&wall->on_pointer_unlock.link);
    wl_list_remove(&wall->on_resize.link);
    wl_list_remove(&wall->on_view_create.link);
    wl_list_remove(&wall->on_view_destroy.link);

    free(wall);
}

int
wall_set_config(struct wall *wall, struct config *cfg) {
    bool counter_updated = strcmp(wall->cfg->general.counter_path, cfg->general.counter_path);
    struct counter *counter = NULL;

    if (counter_updated && strlen(cfg->general.counter_path) > 0) {
        counter = counter_create(cfg->general.counter_path);
        if (!counter) {
            ww_log(LOG_ERROR, "failed to create reset counter");
            return 1;
        }
    }

    struct server_config *server_config = server_config_create(wall->server, cfg);
    if (!server_config) {
        ww_log(LOG_ERROR, "failed to create server config");
        goto fail_server;
    }

    if (wall->cpu) {
        if (cpu_set_config(wall->cpu, cfg) != 0) {
            ww_log(LOG_ERROR, "failed to update cpu config");
            goto fail_cpu;
        }
    }

    server_use_config(wall->server, server_config);
    server_config_destroy(server_config);

    if (counter_updated) {
        if (wall->counter) {
            counter_destroy(wall->counter);
        }
        wall->counter = counter;
    }

    config_api_set_wall(cfg, wall);

    wall->cfg = cfg;
    return 0;

fail_cpu:
    server_config_destroy(server_config);

fail_server:
    if (counter) {
        counter_destroy(counter);
    }
    return 1;
}

int
wall_lua_get_hovered(struct wall *wall) {
    return ON_WALL(wall) ? get_hovered(wall) : -1;
}

int
wall_lua_play(struct wall *wall, int id) {
    ww_assert(id >= 0 && id < wall->num_instances);

    if (wall->active_instance == id) {
        return 1;
    }

    play_instance(wall, id);
    return 0;
}

int
wall_lua_reset_one(struct wall *wall, bool count_reset, int id) {
    ww_assert(id >= 0 && id < wall->num_instances);

    bool ok = instance_reset(wall->instances[id]);
    if (!ok) {
        return 1;
    }

    if (count_reset && wall->counter) {
        counter_increment(wall->counter);
    }

    return 0;
}

int
wall_lua_reset_many(struct wall *wall, bool count_reset, size_t num_ids, int ids[static num_ids]) {
    int successful = 0;

    for (size_t i = 0; i < num_ids; i++) {
        int id = ids[i];
        ww_assert(id >= 0 && id < wall->num_instances);

        if (instance_reset(wall->instances[id])) {
            successful++;
        }
    }

    if (count_reset && wall->counter) {
        wall->counter->count += successful;
        counter_commit(wall->counter);
    }

    return successful;
}

int
wall_lua_return(struct wall *wall) {
    if (ON_WALL(wall)) {
        return 1;
    }

    focus_wall(wall);
    return 0;
}

int
wall_lua_set_res(struct wall *wall, int id, int32_t width, int32_t height) {
    if ((width == 0) != (height == 0)) {
        return 1;
    }

    // TODO: check against buffer transform
    if (id == wall->active_instance) {
        wall->active_res.w = width;
        wall->active_res.h = height;
        layout_active(wall);
    } else {
        struct server_txn *txn = server_txn_create();
        ww_assert(txn);

        struct server_txn_view *view = server_txn_get_view(txn, wall->instances[id]->view);
        ww_assert(view);

        server_txn_view_set_size(view, width, height);

        server_ui_apply(wall->server->ui, txn);
    }

    return 0;
}
