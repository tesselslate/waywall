#include "wall.h"
#include "config.h"
#include "inotify.h"
#include "instance.h"
#include "server/cursor.h"
#include "server/server.h"
#include "util.h"
#include <linux/input-event-codes.h>
#include <sys/inotify.h>

static_assert(BTN_JOYSTICK - BTN_MOUSE == STATIC_ARRLEN((struct wall){0}.buttons));

#define ON_WALL(w) ((w)->active_instance == -1)

struct box {
    int32_t x, y;
    uint32_t w, h;
};

static struct box
get_hitbox(struct wall *wall, int id) {
    int inst_width = wall->server->ui.width / wall->cfg->wall.width;
    int inst_height = wall->server->ui.height / wall->cfg->wall.height;

    struct box box;
    box.x = (id % wall->cfg->wall.width) * inst_width;
    box.y = (id / wall->cfg->wall.width) * inst_height;
    box.w = inst_width;
    box.h = inst_height;
    return box;
}

static int
get_hovered(struct wall *wall) {
    if (wall->mx < 0 || wall->mx >= wall->server->ui.width) {
        return -1;
    }
    if (wall->my < 0 || wall->my >= wall->server->ui.height) {
        return -1;
    }

    int inst_width = wall->server->ui.width / wall->cfg->wall.width;
    int inst_height = wall->server->ui.height / wall->cfg->wall.height;

    int x = wall->mx / inst_width;
    int y = wall->my / inst_height;
    int id = y * wall->cfg->wall.width + x;
    return (id < wall->num_instances) ? id : -1;
}

static void
layout_instance(struct wall *wall, int id) {
    ww_assert(ON_WALL(wall));

    struct box hb = get_hitbox(wall, id);
    server_view_set_dest_size(wall->instances[id]->view, hb.w, hb.h);
    server_view_set_position(wall->instances[id]->view, hb.x, hb.y);
}

static void
layout_wall(struct wall *wall) {
    ww_assert(ON_WALL(wall));

    for (int i = 0; i < wall->num_instances; i++) {
        layout_instance(wall, i);
    }
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

    instance_state_update(wall->instances[id]);
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

    server_view_set_size(instance->view, wall->cfg->wall.stretch_width,
                         wall->cfg->wall.stretch_height);

    if (ON_WALL(wall)) {
        layout_instance(wall, id);
        server_view_show(instance->view);
    }

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

    for (int i = 0; i < wall->num_instances; i++) {
        server_view_show(wall->instances[i]->view);
    }
}

static void
remove_instance(struct wall *wall, int index) {
    instance_destroy(wall->instances[index]);

    memmove(wall + index, wall + index + 1,
            sizeof(struct instance *) * (wall->num_instances - index - 1));
    wall->num_instances--;

    if (ON_WALL(wall)) {
        layout_wall(wall);
    } else if (wall->active_instance == index) {
        focus_wall(wall);
    }
}

static void
play_instance(struct wall *wall, int id) {
    wall->active_instance = id;

    instance_unpause(wall->instances[id]);
    server_set_input_focus(wall->server, wall->instances[id]->view);

    server_view_set_position(wall->instances[id]->view, 0, 0);
    server_view_set_dest_size(wall->instances[id]->view, wall->server->ui.width,
                              wall->server->ui.height);
    server_view_set_size(wall->instances[id]->view, wall->server->ui.width,
                         wall->server->ui.height);

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

    server_set_pointer_pos(wall->server, wall->server->ui.width / 2.0,
                           wall->server->ui.height / 2.0);
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
        layout_wall(wall);
    } else {
        struct server_view *view = wall->instances[wall->active_instance]->view;
        server_view_set_dest_size(view, wall->server->ui.width, wall->server->ui.height);
        server_view_set_size(view, wall->server->ui.width, wall->server->ui.height);
    }

    if (wall->pointer_locked) {
        server_set_pointer_pos(wall->server, wall->server->ui.width / 2.0,
                               wall->server->ui.height / 2.0);
    }
}

static void
on_view_create(struct wl_listener *listener, void *data) {
    struct wall *wall = wl_container_of(listener, wall, on_view_create);
    struct server_view *view = data;

    // TODO: No instance cap
    // Also, I'd like to stop heap allocating all of the instances for cache locality, even though
    // I imagine the bulk of the bad performance will come from libwayland and the server code
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

    button -= BTN_MOUSE;
    if (button >= STATIC_ARRLEN(wall->buttons)) {
        return false;
    }

    wall->buttons[button] = pressed;
    return false;

    // TODO: process input on wall
}

static bool
on_key(void *data, uint32_t key, bool pressed) {
    struct wall *wall = data;

    // TODO: proper keybind handling
    int id = get_hovered(wall);
    if (pressed) {
        switch (key) {
        case KEY_U: // ingame reset
            if (!ON_WALL(wall)) {
                instance_reset(wall->instances[wall->active_instance]);
                server_view_set_size(wall->instances[wall->active_instance]->view,
                                     wall->cfg->wall.stretch_width, wall->cfg->wall.stretch_height);
                focus_wall(wall);
            }
            break;
        case KEY_T: // wall reset all
            if (ON_WALL(wall)) {
                for (int i = 0; i < wall->num_instances; i++) {
                    instance_reset(wall->instances[i]);
                }
            }
            break;
        case KEY_E: // wall reset
            if (id != -1) {
                instance_reset(wall->instances[id]);
            }
            break;
        case KEY_R: // wall play
            if (id != -1) {
                play_instance(wall, id);
            }
            break;
        default:
            break;
        }
    }

    return false;
}

static void
on_motion(void *data, double x, double y) {
    struct wall *wall = data;

    wall->mx = x;
    wall->my = y;

    // TODO: process input on wall
}

static void
on_keymap(void *data, int fd, uint32_t size) {
    // TODO: use the keymap to parse the user's config keybinds
}

static const struct server_seat_listener seat_listener = {
    .button = on_button,
    .key = on_key,
    .motion = on_motion,

    .keymap = on_keymap,
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

    wall->active_instance = -1;

    wall->on_pointer_lock.notify = on_pointer_lock;
    wl_signal_add(&server->events.pointer_lock, &wall->on_pointer_lock);

    wall->on_pointer_unlock.notify = on_pointer_unlock;
    wl_signal_add(&server->events.pointer_unlock, &wall->on_pointer_unlock);

    wall->on_resize.notify = on_resize;
    wl_signal_add(&server->ui.events.resize, &wall->on_resize);

    wall->on_view_create.notify = on_view_create;
    wl_signal_add(&server->ui.events.view_create, &wall->on_view_create);

    wall->on_view_destroy.notify = on_view_destroy;
    wl_signal_add(&server->ui.events.view_destroy, &wall->on_view_destroy);

    server_set_seat_listener(server, &seat_listener, wall);

    return wall;
}

void
wall_destroy(struct wall *wall) {
    for (int i = 0; i < wall->num_instances; i++) {
        inotify_unsubscribe(wall->inotify, wall->instances[i]->state_wd);
        instance_destroy(wall->instances[i]);
    }
    wall->num_instances = 0;

    wl_list_remove(&wall->on_pointer_lock.link);
    wl_list_remove(&wall->on_pointer_unlock.link);
    wl_list_remove(&wall->on_resize.link);
    wl_list_remove(&wall->on_view_create.link);
    wl_list_remove(&wall->on_view_destroy.link);

    free(wall);
}
