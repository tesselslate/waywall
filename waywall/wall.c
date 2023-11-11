#include "wall.h"
#include "compositor.h"
#include "config.h"
#include "cpu.h"
#include "instance.h"
#include "reset_counter.h"
#include "util.h"
#include "waywall.h"
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>

// TODO: reimpl benchmark
// TODO: overhaul keybind matching to be less specific

static size_t
inst_arr_push(struct wall *wall, struct instance instance) {
    ww_assert(wall->instance_count < ARRAY_LEN(wall->instances));
    wall->instances[wall->instance_count] = instance;
    wall->instance_data[wall->instance_count] = (struct wall_instance_data){0};
    wall->instances[wall->instance_count].id = wall->instance_count;
    return wall->instance_count++;
}

static inline void
inst_arr_update(struct wall *wall) {
    for (size_t i = 0; i < wall->instance_count; i++) {
        wall->instances[i].id = i;
    }
}

static void
inst_arr_remove(struct wall *wall, size_t id) {
    ww_assert(id < wall->instance_count);

    for (id++; id < wall->instance_count; id++) {
        wall->instances[id - 1] = wall->instances[id];
        wall->instance_data[id - 1] = wall->instance_data[id];
    }
    inst_arr_update(wall);
    wall->instance_count--;
}

static struct wlr_box
compute_alt_res(struct wall *wall) {
    ww_assert(g_config->has_alt_res);

    return (struct wlr_box){
        .x = (wall->screen_width - g_config->alt_width) / 2,
        .y = (wall->screen_height - g_config->alt_height) / 2,
        .width = g_config->alt_width,
        .height = g_config->alt_height,
    };
}

static int
get_hovered_instance(struct wall *wall) {
    ww_assert(wall->active_instance == -1);

    if (wall->input.cx < 0 || wall->input.cy < 0) {
        return -1;
    }
    if (wall->input.cx >= wall->screen_width || wall->input.cy >= wall->screen_height) {
        return -1;
    }

    int x = wall->input.cx / (wall->screen_width / g_config->wall_width);
    int y = wall->input.cy / (wall->screen_height / g_config->wall_height);
    size_t id = x + y * g_config->wall_width;
    return id >= wall->instance_count ? -1 : (int)id;
}

static struct wlr_box
get_wall_box(struct wall *wall, size_t id) {
    ww_assert(id < wall->instance_count);

    int disp_width = wall->screen_width / g_config->wall_width;
    int disp_height = wall->screen_height / g_config->wall_height;

    struct wlr_box box = {
        .x = disp_width * (id % g_config->wall_width),
        .y = disp_height * (id / g_config->wall_width),
        .width = disp_width,
        .height = disp_height,
    };

    return box;
}

static void
resize_active_instance(struct wall *wall) {
    ww_assert(wall->active_instance != -1);

    struct window *window = wall->instances[wall->active_instance].window;
    struct wlr_box box = wall->alt_res
                             ? compute_alt_res(wall)
                             : (struct wlr_box){0, 0, wall->screen_width, wall->screen_height};
    render_window_configure(window, box.x, box.y, box.width, box.height);
    render_window_set_dest_size(window, box.width, box.height);
}

static void
relayout_instance(struct wall *wall, size_t id) {
    ww_assert(id < wall->instance_count);

    struct wlr_box box = get_wall_box(wall, id);
    render_window_configure(wall->instances[id].window, box.x, box.y, g_config->stretch_width,
                            g_config->stretch_height);
    render_window_set_dest_size(wall->instances[id].window, box.width, box.height);

    render_rect_configure(wall->instance_data[id].lock_indicator, box);
}

static void
relayout_wall(struct wall *wall) {
    for (size_t i = 0; i < wall->instance_count; i++) {
        relayout_instance(wall, i);
    }
}

static void
verif_update_instance(struct wall *wall, size_t id) {
    ww_assert(id < wall->instance_count);

    struct instance *instance = &wall->instances[id];

    // Calculate the actual GUI scale in use.
    // See Minecraft's source (Window.calculateScaleFactor) for the original calculation.
    int i;
    for (i = 1;; i++) {
        // If the window is big enough, the actual GUI scale will equal the GUI scale being used in
        // options. If auto GUI scale is being used, gui_scale is equal to 0, so this check is never
        // true.
        if (i == instance->options.gui_scale) {
            break;
        }

        // We need to check that the window is big enough at each GUI scale.
        bool in_window = i < g_config->stretch_width && i < g_config->stretch_height;
        bool fits_width = g_config->stretch_width / (i + 1) >= 320;
        bool fits_height = g_config->stretch_height / (i + 1) >= 240;
        if (!in_window || !fits_width || !fits_height) {
            break;
        }
    }
    if (instance->options.unicode && i % 2 != 0) {
        i++;
    }

    struct wall_instance_data *data = &wall->instance_data[id];
    ww_assert(data->hview_chunkmap && data->hview_instance);

    int w = HEADLESS_WIDTH / g_config->wall_width, h = HEADLESS_HEIGHT / g_config->wall_height;
    int x = (id % g_config->wall_width) * w, y = (id / g_config->wall_width) * h;

    hview_set_dest(data->hview_instance, (struct wlr_box){x, y, w, h});

    // TODO: take into account whether the user has worldpreview for corner chunkmap
    // TODO: support really weird configurations where the chunkmap is bigger than the instance
    // capture
    // TODO: older versions dont have chunkmaps

    // Calculate the size of the chunkmap capture and configure it accordingly.
    int chunkmap_size = i * 90;
    int progress_height = i * 19;
    int total_height = chunkmap_size + progress_height;
    struct wlr_box chunkmap_src = {
        .x = 0,
        .y = g_config->stretch_height - total_height,
        .width = chunkmap_size,
        .height = total_height,
    };
    hview_set_dest(data->hview_chunkmap,
                   (struct wlr_box){x, y + h - total_height, chunkmap_size, total_height});
    hview_set_src(data->hview_chunkmap, chunkmap_src);
}

// TODO: call this from config update (in case instance sizes changed)
// TODO: call this when options.txt update checking is implemented (for GUI scale changes)
static void
verif_update(struct wall *wall) {
    for (size_t i = 0; i < wall->instance_count; i++) {
        verif_update_instance(wall, i);
    }
}

static void
sleepbg_lock_toggle(bool state) {
    static bool sleepbg_state;

    if (!g_config->sleepbg_lock || sleepbg_state == state) {
        return;
    }

    sleepbg_state = state;
    if (state) {
        int fd = creat(g_config->sleepbg_lock, 0644);
        if (fd == -1) {
            wlr_log_errno(WLR_ERROR, "failed to create sleepbg.lock");
        } else {
            close(fd);
        }
    } else {
        int ret = remove(g_config->sleepbg_lock);
        if (ret == -1) {
            wlr_log_errno(WLR_ERROR, "failed to delete sleepbg.lock");
        }
    }
}

static void
focus_wall(struct wall *wall) {
    ww_assert(wall->active_instance != -1);

    input_focus_window(g_compositor->input, NULL);
    input_set_on_wall(g_compositor->input, true);
    sleepbg_lock_toggle(false);
    wall->active_instance = -1;

    for (size_t i = 0; i < wall->instance_count; i++) {
        render_window_set_enabled(wall->instances[i].window, true);
    }
    render_layer_set_enabled(g_compositor->render, LAYER_LOCKS, true);
}

static bool
play_instance(struct wall *wall, int id) {
    if (!instance_focus(&wall->instances[id])) {
        return false;
    }

    wall->active_instance = id;

    input_set_on_wall(g_compositor->input, false);
    render_window_configure(wall->instances[id].window, 0, 0, wall->screen_width,
                            wall->screen_height);
    render_window_set_dest_size(wall->instances[id].window, wall->screen_width,
                                wall->screen_height);

    if (wall->instance_data[id].locked) {
        wall->instance_data[id].locked = false;
        render_rect_set_enabled(wall->instance_data[id].lock_indicator, false);
    }

    for (size_t i = 0; i < wall->instance_count; i++) {
        render_window_set_enabled(wall->instances[i].window,
                                  (int)i == wall->active_instance ? true : false);
    }
    render_layer_set_enabled(g_compositor->render, LAYER_LOCKS, false);

    sleepbg_lock_toggle(true);

    return true;
}

static void
toggle_locked(struct wall *wall, int id) {
    if (!wall->instance_data[id].locked) {
        wall->instance_data[id].locked = true;
        render_rect_set_enabled(wall->instance_data[id].lock_indicator, true);
    } else {
        switch (g_config->unlock_behavior) {
        case UNLOCK_ACCEPT:
            wall->instance_data[id].locked = false;
            render_rect_set_enabled(wall->instance_data[id].lock_indicator, false);
        case UNLOCK_IGNORE:
            break;
        case UNLOCK_RESET:
            wall->instance_data[id].locked = false;
            render_rect_set_enabled(wall->instance_data[id].lock_indicator, false);

            if (instance_reset(&wall->instances[id])) {
                if (wall->reset_counter) {
                    reset_counter_increment(wall->reset_counter);
                }
            }
            break;
        }
    }
}

static void
process_bind(struct wall *wall, struct keybind *bind) {
    for (int i = 0; i < bind->action_count; i++) {
        enum action action = bind->actions[i];

        bool is_ingame = IS_INGAME_ACTION(action) && wall->active_instance != -1;
        bool is_wall = !IS_INGAME_ACTION(action) && wall->active_instance == -1;
        bool is_any = IS_UNIVERSAL_ACTION(action);

        // waywall is not in a state to perform the given action.
        if (!is_ingame && !is_wall && !is_any) {
            continue;
        }

        int hovered = -1;
        if (is_wall) {
            hovered = get_hovered_instance(wall);
            wall->input.last_bind.bind = bind;
            wall->input.last_bind.instance = hovered;
        }

        switch (action) {
        case ACTION_ANY_TOGGLE_NINB:
            // TODO:
            break;
        case ACTION_WALL_RESET_ALL:
            if (wall->reset_counter) {
                reset_counter_queue_writes(wall->reset_counter);
            }
            for (size_t id = 0; id < wall->instance_count; id++) {
                if (!wall->instance_data[id].locked) {
                    instance_reset(&wall->instances[id]);
                    reset_counter_increment(wall->reset_counter);
                }
            }
            if (wall->reset_counter) {
                reset_counter_commit_writes(wall->reset_counter);
            }
            break;
        case ACTION_WALL_RESET_ONE:
            if (hovered != -1 && !wall->instance_data[hovered].locked) {
                if (instance_reset(&wall->instances[hovered])) {
                    if (wall->reset_counter) {
                        reset_counter_increment(wall->reset_counter);
                    }
                }
            }
            break;
        case ACTION_WALL_PLAY:
            if (hovered != -1) {
                play_instance(wall, hovered);
            }
            break;
        case ACTION_WALL_LOCK:
            if (hovered != -1) {
                toggle_locked(wall, hovered);
            }
            break;
        case ACTION_WALL_FOCUS_RESET:
            if (hovered != -1 && (wall->instances[hovered].state.screen == INWORLD)) {
                if (wall->reset_counter) {
                    reset_counter_queue_writes(wall->reset_counter);
                }
                for (size_t id = 0; id < wall->instance_count; id++) {
                    if ((int)id != hovered && !wall->instance_data[id].locked) {
                        if (instance_reset(&wall->instances[id])) {
                            if (wall->reset_counter) {
                                reset_counter_increment(wall->reset_counter);
                            }
                        }
                    }
                }
                if (wall->reset_counter) {
                    reset_counter_commit_writes(wall->reset_counter);
                }
            }
            break;
        case ACTION_INGAME_RESET:
            instance_reset(&wall->instances[wall->active_instance]);
            relayout_instance(wall, wall->active_instance);
            if (wall->reset_counter) {
                reset_counter_increment(wall->reset_counter);
            }

            if (g_config->wall_bypass) {
                for (size_t id = 0; id < wall->instance_count; id++) {
                    if ((int)id == wall->active_instance) {
                        continue;
                    }
                    if (wall->instance_data[id].locked &&
                        wall->instances[id].state.screen == INWORLD) {
                        if (play_instance(wall, id)) {
                            goto bypass;
                        } else {
                            break;
                        }
                    }
                }
            }

            focus_wall(wall);
        bypass:
            break;
        case ACTION_INGAME_ALT_RES:
            if (!g_config->has_alt_res) {
                break;
            }
            struct window *window = wall->instances[wall->active_instance].window;
            if (wall->alt_res) {
                render_window_configure(window, 0, 0, wall->screen_width, wall->screen_height);
                render_window_set_dest_size(window, wall->screen_width, wall->screen_height);
                input_set_sensitivity(g_compositor->input, g_config->main_sens);
            } else {
                struct wlr_box box = compute_alt_res(wall);
                render_window_configure(window, box.x, box.y, box.width, box.height);
                render_window_set_dest_size(window, box.width, box.height);
                input_set_sensitivity(g_compositor->input, g_config->alt_sens);
            }
            wall->alt_res = !wall->alt_res;
            break;
        }
    }
}

static void
on_button(struct wl_listener *listener, void *data) {
    struct wall *wall = wl_container_of(listener, wall, on_button);
    struct compositor_button_event *event = data;

    int button = event->button - BTN_MOUSE;
    if (button < 0 || button >= (int)ARRAY_LEN(wall->input.buttons)) {
        wlr_log(WLR_ERROR, "received button event for non-mouse button %" PRIu32, event->button);
        return;
    }
    wall->input.buttons[button] = event->state;

    // We only want to process mouse clicks for keybinds, not releases.
    if (!event->state) {
        return;
    }
    for (int i = 0; i < g_config->bind_count; i++) {
        struct keybind bind = g_config->binds[i];
        if (bind.type != BIND_MOUSE) {
            continue;
        }
        if (bind.modifiers != wall->input.modifiers) {
            continue;
        }
        if (bind.input.button != event->button) {
            continue;
        }
        process_bind(wall, &bind);
        return;
    }
}

static void
on_key(struct wl_listener *listener, void *data) {
    struct wall *wall = wl_container_of(listener, wall, on_key);
    struct compositor_key_event *event = data;

    // Do not process key releases.
    if (!event->state) {
        return;
    }
    for (int i = 0; i < g_config->bind_count; i++) {
        struct keybind bind = g_config->binds[i];
        if (bind.type != BIND_KEY) {
            continue;
        }
        if (bind.modifiers != event->modifiers) {
            continue;
        }

        // If needed, do not process the keybind on menus or while paused.
        bool check_inworld = wall->active_instance != -1 &&
                             wall->instances[wall->active_instance].state.screen == INWORLD;
        if (check_inworld) {
            struct state state = wall->instances[wall->active_instance].state;
            if (!bind.allow_in_pause && state.data.inworld == PAUSED) {
                return;
            }
            if (!bind.allow_in_menu && state.data.inworld == MENU) {
                return;
            }
        }

        // Check for any matching keysyms.
        for (int j = 0; j < event->nsyms; j++) {
            if (event->syms[j] == g_config->binds[i].input.sym) {
                process_bind(wall, &bind);
                event->consumed = true;
                return;
            }
        }
    }
}

static void
on_modifiers(struct wl_listener *listener, void *data) {
    struct wall *wall = wl_container_of(listener, wall, on_modifiers);
    xkb_mod_mask_t *mask = data;

    wall->input.modifiers = *mask;
}

static void
on_motion(struct wl_listener *listener, void *data) {
    struct wall *wall = wl_container_of(listener, wall, on_motion);
    struct compositor_motion_event *event = data;

    // Do not process cursor motion while ingame.
    if (wall->active_instance != -1) {
        return;
    }

    wall->input.cx = event->x;
    wall->input.cy = event->y;

    int id = get_hovered_instance(wall);
    if (id == -1) {
        return;
    }

    for (int i = 0; i < g_config->bind_count; i++) {
        struct keybind *bind = &g_config->binds[i];
        if (bind->type != BIND_MOUSE) {
            continue;
        }
        if (bind->modifiers != wall->input.modifiers) {
            continue;
        }
        if (!wall->input.buttons[bind->input.button - BTN_MOUSE]) {
            continue;
        }

        // Allow the user to drag the cursor over several instances without spamming actions.
        bool same_last_instance = wall->input.last_bind.instance == id;
        bool same_last_bind = wall->input.last_bind.bind == &g_config->binds[i];
        if (same_last_instance && same_last_bind) {
            continue;
        }

        process_bind(wall, bind);
        return;
    }
}

static void
on_output_resize(struct wl_listener *listener, void *data) {
    struct wall *wall = wl_container_of(listener, wall, on_output_resize);
    struct output *output = data;

    render_output_get_size(output, &wall->screen_width, &wall->screen_height);
    relayout_wall(wall);
    if (wall->active_instance != -1) {
        resize_active_instance(wall);
    }
}

static void
on_window_map(struct wl_listener *listener, void *data) {
    struct wall *wall = wl_container_of(listener, wall, on_window_map);
    struct window *window = data;

    bool err = false;
    struct instance instance = instance_try_from(window, &err);
    if (err) {
        return;
    }
    if (wall->instance_count == MAX_INSTANCES) {
        wlr_log(WLR_ERROR, "new instance detected but not added (instance limit of " STR(
                               MAX_INSTANCES) " exceeded)");
        return;
    }

    // TODO: Sort instances. Also allow specifying instance sorting IDs

    // Add the instance to the instances array. Zero out the storage in case it was previously in
    // use by another instance.
    size_t id = inst_arr_push(wall, instance);

    // Perform setup for the instance.
    wall->instance_data[id].hview_chunkmap = hview_create(instance.window);
    wall->instance_data[id].hview_instance = hview_create(instance.window);
    wall->instance_data[id].lock_indicator =
        render_rect_create(g_compositor->render, get_wall_box(wall, id), g_config->lock_color);
    render_rect_set_enabled(wall->instance_data[id].lock_indicator, false);
    relayout_instance(wall, id);
    verif_update_instance(wall, id);

    render_window_set_layer(instance.window, LAYER_INSTANCE);
    render_window_set_enabled(instance.window, true);
}

static void
on_window_unmap(struct wl_listener *listener, void *data) {
    struct wall *wall = wl_container_of(listener, wall, on_window_unmap);
    struct window *window = data;

    // Handle any instances dying.
    for (size_t i = 0; i < wall->instance_count; i++) {
        if (wall->instances[i].window == window) {
            if ((int)i == wall->active_instance) {
                focus_wall(wall);
            }

            instance_destroy(&wall->instances[i]);
            hview_destroy(wall->instance_data[i].hview_chunkmap);
            hview_destroy(wall->instance_data[i].hview_instance);
            render_rect_destroy(wall->instance_data[i].lock_indicator);

            inst_arr_remove(wall, i);
            relayout_wall(wall);
            verif_update(wall);
            return;
        }
    }
}

struct wall *
wall_create() {
    ww_assert(g_inotify > 0);
    ww_assert(g_config);
    ww_assert(g_compositor);

    struct wall *wall = calloc(1, sizeof(struct wall));
    ww_assert(wall);

    wall->active_instance = -1;
    input_set_on_wall(g_compositor->input, true);

    if (g_config->has_cpu) {
        if (!cpu_init()) {
            return NULL;
        }
    }

    if (g_config->count_resets) {
        wall->reset_counter = reset_counter_from_file(g_config->resets_file);
        if (!wall->reset_counter) {
            free(wall);
            return NULL;
        }
    }

    wall->on_button.notify = on_button;
    wl_signal_add(&g_compositor->input->events.button, &wall->on_button);

    wall->on_key.notify = on_key;
    wl_signal_add(&g_compositor->input->events.key, &wall->on_key);

    wall->on_modifiers.notify = on_modifiers;
    wl_signal_add(&g_compositor->input->events.modifiers, &wall->on_modifiers);

    wall->on_motion.notify = on_motion;
    wl_signal_add(&g_compositor->input->events.motion, &wall->on_motion);

    wall->on_output_resize.notify = on_output_resize;
    wl_signal_add(&g_compositor->render->events.wl_output_resize, &wall->on_output_resize);

    wall->on_window_map.notify = on_window_map;
    wl_signal_add(&g_compositor->render->events.window_map, &wall->on_window_map);

    wall->on_window_unmap.notify = on_window_unmap;
    wl_signal_add(&g_compositor->render->events.window_unmap, &wall->on_window_unmap);

    return wall;
}

void
wall_destroy(struct wall *wall) {
    // TODO
}

bool
wall_process_inotify(struct wall *wall, const struct inotify_event *event) {
    for (size_t i = 0; i < wall->instance_count; i++) {
        if (instance_process_inotify(&wall->instances[i], event)) {
            return true;
        }
    }
    return false;
}

void
wall_update_config(struct wall *wall) {
    // TODO
}
