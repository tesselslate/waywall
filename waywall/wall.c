#include "wall.h"
#include "compositor/hview.h"
#include "compositor/render.h"
#include "compositor/xwayland.h"
#include "config.h"
#include "cpu.h"
#include "instance.h"
#include "layout.h"
#include "ninb.h"
#include "reset_counter.h"
#include "util.h"
#include "waywall.h"
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <wlr/xwayland.h>

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

static void
update_cpu(struct wall *wall, size_t id, enum cpu_group group) {
    if (!g_config->has_cpu) {
        return;
    }

    if (group == CPU_NONE) {
        switch (wall->instances[id].state.screen) {
        case TITLE:
        case GENERATING:
        case WAITING:
            group = CPU_HIGH;
            break;
        case PREVIEWING:
            if (wall->instance_data[id].locked) {
                group = CPU_HIGH;
            } else if (wall->instances[id].state.data.percent < g_config->preview_threshold) {
                group = CPU_HIGH;
            } else {
                group = CPU_LOW;
            }
            break;
        case INWORLD:
            if (wall->active_instance == (int)id) {
                group = CPU_ACTIVE;
            } else {
                group = CPU_IDLE;
            }
            break;
        default:
            ww_unreachable();
        }
    }

    if (group == wall->instance_data[id].last_group) {
        return;
    }
    wall->instance_data[id].last_group = group;
    cpu_move_to_group(wall->instances[id].window->xwl_window->surface->pid, group);
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

    for (size_t i = 0; i < wall->layout.entry_count; i++) {
        struct layout_entry *entry = &wall->layout.entries[i];
        if (entry->type != INSTANCE) {
            continue;
        }
        if (wall->input.cx >= entry->x && wall->input.cx <= entry->x + entry->w) {
            if (wall->input.cy >= entry->y && wall->input.cy <= entry->y + entry->h) {
                return entry->data.instance;
            }
        }
    }
    return -1;
}

static void
apply_layout(struct wall *wall, struct layout layout) {
    // Destroy the old layout.
    for (size_t i = 0; i < wall->rectangle_count; i++) {
        render_rect_destroy(wall->rectangles[i]);
    }
    for (size_t i = 0; i < wall->instance_count; i++) {
        render_window_set_enabled(wall->instances[i].window, false);
        render_window_set_layer(wall->instances[i].window, LAYER_WALL);
    }
    wall->rectangle_count = 0;
    layout_destroy(wall->layout);

    // TODO: Does z-ordering happen as intended? Might need to manually reorder things

    // Apply the new layout.
    for (size_t i = 0; i < layout.entry_count; i++) {
        struct layout_entry *entry = &layout.entries[i];
        if (entry->type == RECTANGLE) {
            wall->rectangles =
                realloc(wall->rectangles, sizeof(render_rect_t) * (wall->rectangle_count + 1));
            ww_assert(wall->rectangles);
            wall->rectangles[wall->rectangle_count++] = render_rect_create(
                g_compositor->render, (struct wlr_box){entry->x, entry->y, entry->w, entry->h},
                entry->data.color);
        } else if (entry->type == INSTANCE) {
            ww_assert(entry->data.instance >= 0 &&
                      entry->data.instance < (int)wall->instance_count);
            render_window_set_enabled(wall->instances[entry->data.instance].window, true);
            render_window_set_pos(wall->instances[entry->data.instance].window, entry->x, entry->y);
            render_window_set_dest_size(wall->instances[entry->data.instance].window, entry->w,
                                        entry->h);
        } else {
            ww_unreachable();
        }
    }
    wall->layout = layout;
}

static void
relayout_wall(struct wall *wall, struct layout_reason reason) {
    ww_assert(wall->active_instance == -1);
    struct layout layout = {0};
    bool ok = layout_request_new(wall, reason, &layout);
    if (!ok) {
        return;
    }
    apply_layout(wall, layout);
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
    ww_assert(data->hview_instance);

    // TODO: make headless size configurable (pub_compositor.h)
    int w = HEADLESS_WIDTH / 3, h = HEADLESS_HEIGHT / 5;
    int x = (id % 3) * w, y = (id / 5) * h;

    hview_set_dest(data->hview_instance, (struct wlr_box){x, y, w, h});

    // TODO: support really weird configurations where the chunkmap is bigger than the instance
    // capture
    // TODO: handle non-worldpreview (centered chunkmap), and warn when the chunkmap is in 2 spots
    //       (worldpreview < 3.0)

    if (instance->version >= MIN_CHUNKMAP_VERSION) {
        ww_assert(data->hview_chunkmap);

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
}

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

    // We will not necessarily receive any motion events when the pointer is unlocked (hopefully)
    // warped to the center of the window.
    wall->input.cx = wall->screen_width / 2;
    wall->input.cy = wall->screen_height / 2;

    struct layout_reason reason = {REASON_RESET_INGAME, {.instance_id = wall->active_instance}};
    wall->active_instance = -1;
    relayout_wall(wall, reason);
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
    render_window_set_enabled(wall->instances[id].window, true);
    render_window_set_layer(wall->instances[id].window, LAYER_INSTANCE);

    if (wall->instance_data[id].locked) {
        wall->instance_data[id].locked = false;
    }

    for (size_t i = 0; i < wall->instance_count; i++) {
        render_window_set_enabled(wall->instances[i].window,
                                  (int)i == wall->active_instance ? true : false);
    }

    sleepbg_lock_toggle(true);

    return true;
}

static void
reset_instance(struct wall *wall, int id) {
    if (instance_reset(&wall->instances[id])) {
        if (wall->reset_counter) {
            reset_counter_increment(wall->reset_counter);
        }
        update_cpu(wall, (size_t)id, CPU_HIGH);
    }
}

static void
toggle_locked(struct wall *wall, int id) {
    if (!wall->instance_data[id].locked) {
        wall->instance_data[id].locked = true;

        struct layout_reason reason = {REASON_LOCK, {.instance_id = id}};
        relayout_wall(wall, reason);
    } else {
        struct layout_reason reason = {REASON_UNLOCK, {.instance_id = id}};

        switch (g_config->unlock_behavior) {
        case UNLOCK_ACCEPT:
            wall->instance_data[id].locked = false;

            relayout_wall(wall, reason);
            break;
        case UNLOCK_IGNORE:
            break;
        case UNLOCK_RESET:
            wall->instance_data[id].locked = false;
            reset_instance(wall, id);
            relayout_wall(wall, reason);
            break;
        }
    }
    update_cpu(wall, id, CPU_NONE);
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
            ninb_toggle();
            break;
        case ACTION_WALL_RESET_ALL:
            if (wall->reset_counter) {
                reset_counter_queue_writes(wall->reset_counter);
            }
            struct instance_bitfield reset_all = layout_get_reset_all(wall);
            for (size_t id = 0; id < wall->instance_count; id++) {
                if (!wall->instance_data[id].locked && instance_bitfield_has(reset_all, id)) {
                    reset_instance(wall, id);
                }
            }
            if (wall->reset_counter) {
                reset_counter_commit_writes(wall->reset_counter);
            }

            struct layout_reason reason = {.cause = REASON_RESET_ALL};
            relayout_wall(wall, reason);
            break;
        case ACTION_WALL_RESET_ONE:
            if (hovered != -1 && !wall->instance_data[hovered].locked) {
                reset_instance(wall, hovered);

                struct layout_reason reason = {REASON_RESET, {.instance_id = hovered}};
                relayout_wall(wall, reason);
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
                        reset_instance(wall, id);
                    }
                }
                if (wall->reset_counter) {
                    reset_counter_commit_writes(wall->reset_counter);
                }
            }
            break;
        case ACTION_INGAME_RESET:
            render_window_configure(wall->instances[wall->active_instance].window, 0, 0,
                                    g_config->stretch_width, g_config->stretch_height);
            reset_instance(wall, wall->active_instance);

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
        if (wall->active_instance != -1) {
            struct state state = wall->instances[wall->active_instance].state;
            if (!bind.allow_in_pause && state.screen == INWORLD && state.data.inworld == PAUSED) {
                return;
            }
            if (!bind.allow_in_menu) {
                if (state.screen == TITLE) {
                    return;
                }
                if (state.screen == INWORLD && state.data.inworld == MENU) {
                    return;
                }
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
    if (wall->active_instance != -1) {
        resize_active_instance(wall);
    } else {
        struct layout_reason reason = {REASON_RESIZE,
                                       {.screen_size = {wall->screen_width, wall->screen_height}}};
        relayout_wall(wall, reason);
    }
}

static void
on_window_map(struct wl_listener *listener, void *data) {
    struct wall *wall = wl_container_of(listener, wall, on_window_map);
    struct window *window = data;

    bool err = false;
    struct instance instance = instance_try_from(window, &err);
    if (err) {
        if (!ninb_try_window(window)) {
            const char *name = window->xwl_window->surface->title;
            wlr_log(WLR_INFO, "unknown window opened (pid %d, name '%s') - killing",
                    window->xwl_window->surface->pid, name ? name : "unnamed");
            xcb_kill_client(window->xwl_window->xwl->xcb, window->xwl_window->surface->window_id);
        }
        return;
    }
    if (wall->instance_count == MAX_INSTANCES) {
        wlr_log(WLR_ERROR, "new instance detected but not added (instance limit of " STR(
                               MAX_INSTANCES) " exceeded)");
        return;
    }

    // Add the instance to the instances array. Zero out the storage in case it was previously in
    // use by another instance.
    size_t id = inst_arr_push(wall, instance);

    // Perform setup for the instance.
    if (instance.version >= MIN_CHUNKMAP_VERSION) {
        wall->instance_data[id].hview_chunkmap = hview_create(instance.window);
    }
    wall->instance_data[id].hview_instance = hview_create(instance.window);
    if (wall->active_instance == -1) {
        struct layout_reason reason = {REASON_INSTANCE_ADD, {.instance_id = id}};
        relayout_wall(wall, reason);
    }
    verif_update_instance(wall, id);
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
            if (wall->instance_data[i].hview_chunkmap) {
                hview_destroy(wall->instance_data[i].hview_chunkmap);
            }
            hview_destroy(wall->instance_data[i].hview_instance);

            inst_arr_remove(wall, i);
            if (wall->active_instance == -1) {
                struct layout_reason reason = {REASON_INSTANCE_DIE, {.instance_id = i}};
                relayout_wall(wall, reason);
            }
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

    struct layout layout;
    if (!layout_init(wall, &layout)) {
        goto fail_layout;
    }
    apply_layout(wall, layout);

    if (g_config->has_cpu) {
        if (!cpu_init()) {
            goto fail_cpu;
        }
        if (!cpu_set_group_weight(CPU_IDLE, g_config->idle_cpu)) {
            goto fail_cpu;
        }
        if (!cpu_set_group_weight(CPU_LOW, g_config->low_cpu)) {
            goto fail_cpu;
        }
        if (!cpu_set_group_weight(CPU_HIGH, g_config->high_cpu)) {
            goto fail_cpu;
        }
        if (!cpu_set_group_weight(CPU_ACTIVE, g_config->active_cpu)) {
            goto fail_cpu;
        }
    }

    if (g_config->count_resets) {
        wall->reset_counter = reset_counter_from_file(g_config->resets_file);
        if (!wall->reset_counter) {
            goto fail_reset_counter;
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

fail_reset_counter:
    cpu_fini();

fail_cpu:
    layout_fini();

fail_layout:
    free(wall);
    return NULL;
}

void
wall_destroy(struct wall *wall) {
    if (wall->reset_counter) {
        wlr_log(WLR_INFO, "finished counting resets (%d)",
                reset_counter_get_count(wall->reset_counter));
        reset_counter_destroy(wall->reset_counter);
    }
    cpu_fini();
}

bool
wall_process_inotify(struct wall *wall, const struct inotify_event *event) {
    for (size_t i = 0; i < wall->instance_count; i++) {
        bool was_preview = wall->instances[i].state.screen == PREVIEWING;

        if (instance_process_inotify(&wall->instances[i], event)) {
            update_cpu(wall, i, CPU_NONE);

            bool is_preview = wall->instances[i].state.screen == PREVIEWING;
            if (wall->active_instance == -1 && !was_preview && is_preview) {
                struct layout_reason reason = {REASON_PREVIEW_START, {.instance_id = i}};
                relayout_wall(wall, reason);
            }

            return true;
        }
    }
    return false;
}

bool
wall_update_config(struct wall *wall) {
    struct layout layout;
    if (!layout_reinit(wall, &layout)) {
        return false;
    }
    apply_layout(wall, layout);

    if (g_config->count_resets) {
        ww_assert(g_config->resets_file);
        if (wall->reset_counter) {
            if (!reset_counter_change_file(wall->reset_counter, g_config->resets_file)) {
                wlr_log(WLR_ERROR, "failed to change reset count file");
                return false;
            }
        } else {
            wall->reset_counter = reset_counter_from_file(g_config->resets_file);
            if (!wall->reset_counter) {
                wlr_log(WLR_ERROR, "failed to create reset counter");
                return false;
            }
        }
    } else {
        if (wall->reset_counter) {
            int count = reset_counter_get_count(wall->reset_counter);
            wlr_log(WLR_INFO, "disabling reset counting (stopping at %d resets)", count);
            reset_counter_destroy(wall->reset_counter);
            wall->reset_counter = NULL;
        }
    }

    verif_update(wall);

    if (wall->active_instance != -1) {
        resize_active_instance(wall);
    }

    if (wall->active_instance != -1 && wall->alt_res) {
        input_set_sensitivity(g_compositor->input, g_config->alt_sens);
    } else {
        input_set_sensitivity(g_compositor->input, g_config->main_sens);
    }

    return true;
}
