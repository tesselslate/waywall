#include "wall.h"
#include "config/action.h"
#include "config/config.h"
#include "config/layout.h"
#include "counter.h"
#include "inotify.h"
#include "instance.h"
#include "server/cursor.h"
#include "server/server.h"
#include "server/ui.h"
#include "server/wl_seat.h"
#include "util.h"
#include <linux/input-event-codes.h>
#include <sys/inotify.h>
#include <xkbcommon/xkbcommon.h>

static_assert(BTN_JOYSTICK - BTN_MOUSE == STATIC_ARRLEN((struct wall){0}.buttons));

#define ON_WALL(w) ((w)->active_instance == -1)

static int
get_hovered(struct wall *wall) {
    if (!wall->layout) {
        return -1;
    }

    for (size_t i = 0; i < wall->layout->num_elements; i++) {
        struct config_layout_element *element = &wall->layout->elements[i];
        if (element->type == LAYOUT_ELEMENT_INSTANCE) {
            bool x = element->x <= wall->mx && element->x + element->w >= wall->mx;
            bool y = element->y <= wall->my && element->y + element->h >= wall->my;
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

    struct server_view *view = wall->instances[wall->active_instance]->view;

    if (wall->active_res.w == 0) {
        ww_assert(wall->active_res.h == 0);

        server_view_set_position(view, 0, 0);
        server_view_set_dest_size(view, wall->server->ui->width, wall->server->ui->height);
        server_view_set_size(view, wall->server->ui->width, wall->server->ui->height);
        server_view_unset_crop(view);
    } else {
        int32_t x = (wall->server->ui->width / 2) - (wall->active_res.w / 2);
        int32_t y = (wall->server->ui->height / 2) - (wall->active_res.h / 2);

        if (x >= 0 && y >= 0) {
            server_view_set_position(view, x, y);
            server_view_set_dest_size(view, wall->active_res.w, wall->active_res.h);
            server_view_set_size(view, wall->active_res.w, wall->active_res.h);
            server_view_unset_crop(view);
        } else {
            // Negative X or Y coordinates mean that the provided resolution is greater than the
            // size of the waywall window. In this case, we need to crop the view.
            int32_t w = (x >= 0) ? wall->active_res.w : wall->server->ui->width;
            int32_t h = (y >= 0) ? wall->active_res.h : wall->server->ui->height;

            int32_t crop_x = (wall->active_res.w / 2) - (w / 2);
            int32_t crop_y = (wall->active_res.h / 2) - (h / 2);

            x = x >= 0 ? x : 0;
            y = y >= 0 ? y : 0;

            // TODO: I think the compositor can throw a protocol error here when we do the crop? See
            // the documentation for wp_viewport:
            // > If src_x or src_y are negative, the bad_value protocol error is raised. Otherwise,
            // > if the source rectangle is partially or completely outside of the non-NULL
            // > wl_buffer, then the out_of_buffer protocol error is raised when the surface state
            // > is applied. A NULL wl_buffer does not raise the out_of_buffer error.
            //
            // All of the subsurface logic will need an overhaul eventually anyway for the fabled
            // Wayland ~frame perfection~

            server_view_set_position(view, x, y);
            server_view_set_dest_size(view, w, h);
            server_view_set_size(view, wall->active_res.w, wall->active_res.h);
            server_view_set_crop(view, crop_x, crop_y, w, h);
        }
    }
}

static void
layout_wall(struct wall *wall) {
    ww_assert(ON_WALL(wall));

    if (!wall->layout) {
        return;
    }

    bool shown[MAX_INSTANCES] = {0};
    for (size_t i = 0; i < wall->layout->num_elements; i++) {
        struct config_layout_element *element = &wall->layout->elements[i];
        switch (element->type) {
        case LAYOUT_ELEMENT_INSTANCE:
            ww_assert(element->data.instance >= 0 && element->data.instance < wall->num_instances);

            struct server_view *view = wall->instances[element->data.instance]->view;
            server_view_set_dest_size(view, element->w, element->h);
            server_view_set_position(view, element->x, element->y);
            server_view_show(view);
            shown[element->data.instance] = true;
        }
    }

    for (int i = 0; i < wall->num_instances; i++) {
        if (!shown[i]) {
            server_view_hide(wall->instances[i]->view);
        }
    }
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
    // layout generator. In this case, we may need to shift some instance IDs in the layout to
    // ensure that it can continue being used.
    if (!wall->layout) {
        return;
    }

    for (size_t i = 0; i < wall->layout->num_elements; i++) {
        struct config_layout_element *elem = &wall->layout->elements[i];
        if (elem->type == LAYOUT_ELEMENT_INSTANCE) {
            if (elem->data.instance > id) {
                elem->data.instance--;
            }
        }
    }
}

static bool
process_action(struct wall *wall, struct config_action action) {
    bool consumed = (config_action_try(wall->cfg, wall, action) != 0);

    if (consumed) {
        struct config_layout *layout = config_layout_request_manual(wall->cfg, wall);
        change_layout(wall, layout);
    }

    return consumed;
}

static void
process_state_update(int wd, uint32_t mask, void *data) {
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

    if (screen != SCREEN_PREVIEWING && wall->instances[id]->state.screen == SCREEN_PREVIEWING) {
        struct config_layout *layout = config_layout_request_preview_start(wall->cfg, wall, id);
        change_layout(wall, layout);
    } else if (wall->instances[id]->state.screen == SCREEN_PREVIEWING &&
               percent != wall->instances[id]->state.data.percent) {
        struct config_layout *layout = config_layout_request_preview_percent(
            wall->cfg, wall, id, wall->instances[id]->state.data.percent);
        change_layout(wall, layout);
    }
}

static void
add_instance(struct wall *wall, struct instance *instance) {
    char *state_path = instance_get_state_path(instance);
    if (!state_path) {
        ww_log(LOG_ERROR, "failed to get instance state path");
        goto fail_state_path;
    }

    int wd = inotify_subscribe(wall->inotify, state_path, IN_MODIFY, process_state_update, wall);
    if (wd == -1) {
        ww_log(LOG_ERROR, "failed to watch instance state");
        goto fail_wd;
    }
    instance->state_wd = wd;

    int id = wall->num_instances;
    wall->instances[wall->num_instances++] = instance;

#warning TODO stretch

    struct config_layout *layout = config_layout_request_spawn(wall->cfg, wall, id);
    change_layout(wall, layout);

    free(state_path);
    return;

fail_wd:
    free(state_path);

fail_state_path:
    instance_destroy(instance);
}

static void
focus_wall(struct wall *wall) {
    ww_assert(!ON_WALL(wall));

    wall->active_instance = -1;
    server_set_input_focus(wall->server, NULL);

    layout_wall(wall);
}

static void
remove_instance(struct wall *wall, int id) {
    instance_destroy(wall->instances[id]);

    memmove(wall + id, wall + id + 1, sizeof(struct instance *) * (wall->num_instances - id - 1));
    wall->num_instances--;

    if (wall->active_instance == id) {
        focus_wall(wall);
    }

    struct config_layout *layout = config_layout_request_death(wall->cfg, wall, id);
    if (layout) {
        change_layout(wall, layout);
    } else {
        fixup_layout(wall, id);
    }
}

static void
play_instance(struct wall *wall, int id) {
    wall->active_instance = id;

    instance_unpause(wall->instances[id]);
    server_set_input_focus(wall->server, wall->instances[id]->view);

    server_view_set_position(wall->instances[id]->view, 0, 0);
    server_view_set_dest_size(wall->instances[id]->view, wall->server->ui->width,
                              wall->server->ui->height);
    server_view_set_size(wall->instances[id]->view, wall->server->ui->width,
                         wall->server->ui->height);

    for (int i = 0; i < wall->num_instances; i++) {
        if (i == id) {
            server_view_show(wall->instances[id]->view);
        } else {
            server_view_hide(wall->instances[id]->view);
        }
    }
}

static void
on_pointer_lock(struct wl_listener *listener, void *data) {
    struct wall *wall = wl_container_of(listener, wall, on_pointer_lock);
    server_cursor_hide(wall->server->cursor);
    wall->pointer_locked = true;

    server_set_pointer_pos(wall->server, wall->server->ui->width / 2.0,
                           wall->server->ui->height / 2.0);
}

static void
on_pointer_unlock(struct wl_listener *listener, void *data) {
    struct wall *wall = wl_container_of(listener, wall, on_pointer_unlock);
    server_cursor_show(wall->server->cursor);
    wall->pointer_locked = false;
}

static void
on_resize(struct wl_listener *listener, void *data) {
    struct wall *wall = wl_container_of(listener, wall, on_resize);

    if (ON_WALL(wall)) {
        struct config_layout *layout = config_layout_request_resize(
            wall->cfg, wall, wall->server->ui->width, wall->server->ui->height);
        change_layout(wall, layout);
    } else {
        layout_active(wall);
    }

    if (wall->pointer_locked) {
        server_set_pointer_pos(wall->server, wall->server->ui->width / 2.0,
                               wall->server->ui->height / 2.0);
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

    if (button - BTN_MOUSE >= STATIC_ARRLEN(wall->buttons)) {
        return false;
    }

    wall->buttons[button - BTN_MOUSE] = pressed;

    if (!ON_WALL(wall)) {
        return false;
    }

    if (pressed) {
        struct config_action action = {0};
        action.type = CONFIG_ACTION_BUTTON;
        action.data = button;
        action.modifiers = wall->modifiers;

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
        action.modifiers = wall->modifiers;

        return process_action(wall, action);
    } else {
        return false;
    }
}

static void
on_modifiers(void *data, uint32_t mods, uint32_t group) {
    struct wall *wall = data;

    wall->modifiers = mods;
    wall->group = group;
}

static void
on_motion(void *data, double x, double y) {
    struct wall *wall = data;

    wall->mx = x;
    wall->my = y;

    if (!ON_WALL(wall)) {
        return;
    }

    for (size_t i = 0; i < STATIC_ARRLEN(wall->buttons); i++) {
        if (wall->buttons[i]) {
            struct config_action action = {0};
            action.type = CONFIG_ACTION_BUTTON;
            action.data = i + BTN_MOUSE;
            action.modifiers = wall->modifiers;

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
    struct wall *wall = calloc(1, sizeof(*wall));
    if (!wall) {
        ww_log(LOG_ERROR, "failed to allocate wall");
        return NULL;
    }

    wall->cfg = cfg;
    wall->server = server;
    wall->inotify = inotify;

    if (strcmp(cfg->general.counter_path, "") != 0) {
        wall->counter = counter_create(cfg->general.counter_path);
        if (!wall->counter) {
            ww_log(LOG_ERROR, "failed to create reset counter");
            free(wall);
            return NULL;
        }
    }

    wall->active_instance = -1;

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

    if (wall->counter) {
        counter_destroy(wall->counter);
    }

    wl_list_remove(&wall->on_pointer_lock.link);
    wl_list_remove(&wall->on_pointer_unlock.link);
    wl_list_remove(&wall->on_resize.link);
    wl_list_remove(&wall->on_view_create.link);
    wl_list_remove(&wall->on_view_destroy.link);

    free(wall);
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
wall_lua_reset_one(struct wall *wall, int id) {
    ww_assert(id >= 0 && id < wall->num_instances);

    bool ok = instance_reset(wall->instances[id]);
    if (!ok) {
        return 1;
    }

    if (wall->counter) {
        counter_increment(wall->counter);
    }

    return 0;
}

int
wall_lua_reset_many(struct wall *wall, size_t num_ids, int ids[static num_ids]) {
    int successful = 0;

    for (size_t i = 0; i < num_ids; i++) {
        int id = ids[i];
        ww_assert(id >= 0 && id < wall->num_instances);

        if (instance_reset(wall->instances[id])) {
            successful++;
        }
    }

    if (wall->counter) {
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
wall_lua_set_active_res(struct wall *wall, int32_t width, int32_t height) {
    if (ON_WALL(wall)) {
        return 1;
    }

    if ((width == 0) != (height == 0)) {
        return 1;
    }

    // TODO: check against buffer transform
    wall->active_res.w = width;
    wall->active_res.h = height;

    layout_active(wall);
    return 0;
}
