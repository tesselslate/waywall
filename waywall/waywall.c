#include "compositor.h"
#include "config.h"
#include "cpu.h"
#include "instance.h"
#include "reset_counter.h"
#include "util.h"
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/log.h>

// TODO: better benchmarking (config options?)
// TODO: improve string handling in options/mod gathering
// TODO: improve error handling (prevent fd leaks, etc) in instance creation

// TODO: make config reloading reliable (reads file before written sometimes? also might not handle
// some editors correctly)
// TODO: reread instance options on update reliably

#define BENCHMARK_RESET_COUNT 2000
#define WALL -1
#define WAYWALL_DISPLAY_PATH "/tmp/waywall-display"

static char *config_path;
static struct config *config;
static struct compositor *compositor;
static struct wl_event_loop *event_loop;
static int inotify_fd;
static int config_wd;

static struct instance instances[MAX_INSTANCE_COUNT];
static int max_instance_id;
static int active_instance = WALL;
static int32_t screen_width, screen_height;
static struct window *ninb_window;
static bool ninb_shown;

static int cursor_x, cursor_y;
static uint32_t held_modifiers;
static bool held_buttons[8]; // NOTE: Button count is a bit low here, but this supports all current
                             // mouse buttons.
static int held_buttons_count;
static struct {
    int instance;
    struct keybind *bind;
} last_held;

static struct reset_counter *reset_counter;

static struct {
    bool running;
    int resets;
    struct timespec start;
} benchmark;

static void benchmark_finish();
static void benchmark_toggle();
static void benchmark_start();
static void cpu_update_instance(struct instance *, enum cpu_group);
static void config_update();
static struct wlr_box compute_alt_res();
static struct compositor_config create_compositor_config();
static void ninb_reposition(int, int);
static void ninb_set_visible(bool);
static void sleepbg_lock_toggle(bool);
static struct instance *instance_get_hovered();
static inline int instance_get_id(struct instance *);
static struct wlr_box instance_get_wall_box(struct instance *);
static void instance_handle_death(struct instance *);
static void instance_lock(struct instance *);
static void instance_pause(struct instance *);
static void instance_play(struct instance *);
static bool instance_reset(struct instance *);
static void instance_send_reset_keys(struct instance *);
static void instance_update_verification(struct instance *);
static void wall_focus();
static void wall_resize_instance(struct instance *);
static void process_bind(struct keybind *, bool);
static void process_state(struct instance *);
static void process_resize(int32_t, int32_t);
static int handle_signal(int, void *);
static int handle_inotify(int, uint32_t, void *);

static void
benchmark_finish() {
    ww_assert(benchmark.running);

    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    uint64_t start_nsec = benchmark.start.tv_sec * 1000000000 + benchmark.start.tv_nsec;
    uint64_t end_nsec = end.tv_sec * 1000000000 + end.tv_nsec;
    uint64_t ms_diff = (end_nsec - start_nsec) / 1000000;
    wlr_log(WLR_INFO, "benchmark finished with %d resets in %.2lf sec", benchmark.resets,
            (double)ms_diff / 1000.0);
    benchmark.running = false;
}

static void
benchmark_toggle() {
    if (benchmark.running) {
        benchmark_finish();
    } else {
        benchmark_start();
    }
}

static void
benchmark_start() {
    ww_assert(!benchmark.running);

    wlr_log(WLR_INFO, "started benchmark");
    benchmark.resets = 0;
    clock_gettime(CLOCK_MONOTONIC, &benchmark.start);
    benchmark.running = true;

    for (int i = 0; i < max_instance_id; i++) {
        if (instances[i].alive && !instances[i].locked) {
            instance_reset(&instances[i]);
        }
    }
}

static void
cpu_update_instance(struct instance *instance, enum cpu_group override) {
    if (!config->has_cpu) {
        return;
    }
    enum cpu_group group;
    if (override != CPU_NONE) {
        group = override;
    } else {
        switch (instance->state.screen) {
        case TITLE:
            group = CPU_HIGH;
            break;
        case GENERATING:
            group = CPU_HIGH;
            break;
        case WAITING:
            group = CPU_HIGH;
            break;
        case PREVIEWING:
            if (instance->locked || instance->state.data.percent < config->preview_threshold) {
                group = CPU_HIGH;
            } else {
                group = CPU_LOW;
            }
            break;
        case INWORLD:
            if (active_instance == instance_get_id(instance)) {
                group = CPU_ACTIVE;
            } else {
                group = CPU_IDLE;
            }
            break;
        default:
            ww_unreachable();
        }
    }
    if (group == instance->last_group) {
        return;
    }
    instance->last_group = group;
    cpu_move_to_group(window_get_pid(instance->window), group);
}

static void
config_update() {
    wlr_log(WLR_INFO, "configuration file was updated");
    struct config *new_config = config_read();
    if (!new_config) {
        return;
    }

    if (new_config->count_resets) {
        ww_assert(new_config->resets_file);

        if (reset_counter) {
            if (!reset_counter_change_file(reset_counter, new_config->resets_file)) {
                wlr_log(WLR_ERROR, "failed to change reset count file");
            }
        } else {
            reset_counter = reset_counter_from_file(new_config->resets_file);
            if (!reset_counter) {
                wlr_log(WLR_ERROR, "failed to create reset counter");
            }
        }
    } else {
        if (reset_counter) {
            int count = reset_counter_get_count(reset_counter);
            wlr_log(WLR_INFO, "disabling reset counting (stopping at %d resets)", count);
            reset_counter_destroy(reset_counter);
            reset_counter = NULL;
        }
    }

    config_destroy(config);
    config = new_config;

    // Apply changes from the new configuration.
    compositor_load_config(compositor, create_compositor_config());
    for (unsigned long i = 0; i < ARRAY_LEN(instances); i++) {
        if (instances[i].lock_indicator) {
            render_rect_set_color(instances[i].lock_indicator, config->lock_color);
        }
        if (instances[i].alive) {
            instance_update_verification(&instances[i]);
        }
    }
    process_resize(screen_width, screen_height);
    if (active_instance != WALL && instances[active_instance].alt_res) {
        input_set_sensitivity(compositor->input, config->alt_sens);
    } else {
        input_set_sensitivity(compositor->input, config->main_sens);
    }
    if (ninb_window) {
        ninb_reposition(0, 0);
    }

    wlr_log(WLR_INFO, "applied new config");
}

static struct wlr_box
compute_alt_res() {
    ww_assert(config->has_alt_res);

    return (struct wlr_box){
        .x = (screen_width - config->alt_width) / 2,
        .y = (screen_height - config->alt_height) / 2,
        .width = config->alt_width,
        .height = config->alt_height,
    };
}

static struct compositor_config
create_compositor_config() {
    struct compositor_config compositor_config = {
        .repeat_rate = config->repeat_rate,
        .repeat_delay = config->repeat_delay,
        .floating_opacity = config->ninb_opacity,
        .confine_pointer = config->confine_pointer,
        .cursor_theme = config->cursor_theme,
        .cursor_size = config->cursor_size,
        .stop_on_close = !config->remain_in_background,
    };
    memcpy(compositor_config.background_color, config->background_color, sizeof(float) * 4);
    return compositor_config;
}

static void
ninb_reposition(int w, int h) {
    ww_assert(ninb_window);

    if (w <= 0 || h <= 0) {
        render_window_get_size(ninb_window, &w, &h);
    }
    struct wlr_box size = {0, 0, w, h};

    int x, y;
    enum ninb_location loc = config->ninb_location;
    if (loc == TOP_LEFT || loc == LEFT || loc == BOTTOM_LEFT) {
        x = 0;
    } else if (loc == TOP_RIGHT || loc == RIGHT || loc == BOTTOM_RIGHT) {
        x = screen_width - size.width;
    } else if (loc == TOP) {
        x = (screen_width - size.width) / 2;
    } else {
        ww_unreachable();
    }
    if (loc == TOP_LEFT || loc == TOP || loc == TOP_RIGHT) {
        y = 0;
    } else if (loc == LEFT || loc == RIGHT) {
        y = (screen_height - size.height) / 2;
    } else if (loc == BOTTOM_LEFT || loc == BOTTOM_RIGHT) {
        y = screen_height - size.height;
    } else {
        ww_unreachable();
    }
    render_window_set_pos(ninb_window, x, y);
}

static void
ninb_set_visible(bool visible) {
    ninb_shown = visible;
    if (ninb_window) {
        ninb_reposition(0, 0);
    }
    render_layer_set_enabled(compositor->render, LAYER_FLOATING, visible);
}

static void
sleepbg_lock_toggle(bool state) {
    static bool sleepbg_state;
    if (!config->sleepbg_lock || sleepbg_state == state) {
        return;
    }

    sleepbg_state = state;
    if (state) {
        int fd = creat(config->sleepbg_lock, 0644);
        if (fd == -1) {
            wlr_log_errno(WLR_ERROR, "failed to create sleepbg.lock");
        }
        close(fd);
    } else {
        int ret = remove(config->sleepbg_lock);
        if (ret == -1) {
            wlr_log_errno(WLR_ERROR, "failed to delete sleepbg.lock");
        }
    }
}

static struct instance *
instance_get_hovered() {
    if (active_instance != WALL) {
        return NULL;
    }
    if (cursor_x < 0 || cursor_x > screen_width || cursor_y < 0 || cursor_y > screen_height) {
        return NULL;
    }

    int x = cursor_x / (screen_width / config->wall_width);
    int y = cursor_y / (screen_height / config->wall_height);
    int id = x + y * config->wall_width;
    if (!instances[id].alive || id >= max_instance_id) {
        return NULL;
    }
    return &instances[id];
}

static inline int
instance_get_id(struct instance *instance) {
    ww_assert(instance);

    return instance - instances;
}

static struct wlr_box
instance_get_wall_box(struct instance *instance) {
    ww_assert(instance);

    int id = instance_get_id(instance);
    int disp_width = screen_width / config->wall_width;
    int disp_height = screen_height / config->wall_height;

    struct wlr_box box = {
        .x = disp_width * (id % config->wall_width),
        .y = disp_height * (id / config->wall_width),
        .width = disp_width,
        .height = disp_height,
    };
    return box;
}

static void
instance_handle_death(struct instance *instance) {
    wlr_log(WLR_ERROR, "instance %d died", instance_get_id(instance));
    instance->alive = false;
    instance->window = NULL;
    instance->last_group = CPU_NONE;
    free(instance->dir);
    instance->dir = NULL;
    inotify_rm_watch(inotify_fd, instance->state_wd);
    close(instance->state_fd);
    if (instance_get_id(instance) == active_instance) {
        cpu_unset_active();
        input_set_sensitivity(compositor->input, config->main_sens);
        instance->alt_res = false;
        wall_focus();
    }
}

static void
instance_lock(struct instance *instance) {
    ww_assert(instance);
    ww_assert(instance->alive);
    ww_assert(active_instance == WALL);

    if (!instance->locked) {
        // Lock the instance.
        instance->locked = true;
        if (!instance->lock_indicator) {
            struct wlr_box box = instance_get_wall_box(instance);
            instance->lock_indicator =
                render_rect_create(compositor->render, box, config->lock_color);
        }
        render_rect_set_enabled(instance->lock_indicator, instance->locked);
    } else {
        // Unlock the instance.
        ww_assert(instance->lock_indicator);

        switch (config->unlock_behavior) {
        case UNLOCK_ACCEPT:
            instance->locked = false;
            render_rect_set_enabled(instance->lock_indicator, false);
            break;
        case UNLOCK_IGNORE:
            break;
        case UNLOCK_RESET:
            instance->locked = false;
            render_rect_set_enabled(instance->lock_indicator, false);
            instance_reset(instance);
            break;
        }
    }
}

static void
instance_pause(struct instance *instance) {
    ww_assert(instance);
    ww_assert(instance->alive);

    static const struct synthetic_key keys[] = {
        {KEY_F3, true},
        {KEY_ESC, true},
        {KEY_ESC, false},
        {KEY_F3, false},
    };
    input_send_keys(instance->window, keys, ARRAY_LEN(keys));
}

static void
instance_play(struct instance *instance) {
    ww_assert(instance);
    ww_assert(instance->alive);
    ww_assert(instance_get_id(instance) != active_instance);

    // Focus and fullscreen the instance.
    input_set_on_wall(compositor->input, false);
    input_focus_window(compositor->input, instance->window);
    render_window_configure(instance->window, 0, 0, screen_width, screen_height);
    render_window_set_dest_size(instance->window, screen_width, screen_height);

    // Unpause the instance and hide all of the other scene elements.
    static const struct synthetic_key unpause_keys[] = {
        {KEY_ESC, true},
        {KEY_ESC, false},
        {KEY_F1, true},
        {KEY_F1, false},
    };
    input_send_keys(instance->window, unpause_keys,
                    config->use_f1 ? ARRAY_LEN(unpause_keys) : ARRAY_LEN(unpause_keys) - 2);

    active_instance = instance_get_id(instance);
    if (instance->locked) {
        ww_assert(instance->lock_indicator);
        render_rect_set_enabled(instance->lock_indicator, false);
        instance->locked = false;
    }
    for (int i = 0; i < max_instance_id; i++) {
        if (instances[i].alive) {
            render_window_set_enabled(instances[i].window, i == active_instance ? true : false);
        }
    }
    render_layer_set_enabled(compositor->render, LAYER_LOCKS, false);

    // Enable sleepbg.lock.
    sleepbg_lock_toggle(true);
}

static bool
instance_reset(struct instance *instance) {
    ww_assert(instance);
    ww_assert(instance->alive);

    // Do not allow resets on the dirt screen.
    if (instance->state.screen == GENERATING || instance->state.screen == WAITING) {
        return false;
    }

    // Do not allow resets in grace period.
    if (instance->state.screen != INWORLD && config->grace_period > 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        uint64_t preview_msec =
            instance->last_preview.tv_sec * 1000 + instance->last_preview.tv_nsec / 1000000;
        uint64_t now_msec = now.tv_sec * 1000 + instance->last_preview.tv_nsec / 1000000;
        if (now_msec - preview_msec < (uint64_t)config->grace_period) {
            return false;
        }
    }

    // If the instance is still on the title screen, send a fake mouse click. This is necessary
    // because Atum refuses to reset until the window has been clicked once for some reason.
    if (instance->state.screen == TITLE) {
        input_click(instance->window);
    }

    // If the instance is currently being played, try to fix ghost pie.
    if (instance_get_id(instance) == active_instance && instance->state.screen == INWORLD) {
        static const struct synthetic_key ghost_pie_keys[] = {
            {KEY_ESC, true}, {KEY_ESC, false}, {KEY_LEFTSHIFT, false},
            {KEY_F3, true},  {KEY_F3, false},
        };

        if (instance->state.data.world == UNPAUSED) {
            input_send_keys(instance->window, ghost_pie_keys + 2, ARRAY_LEN(ghost_pie_keys) - 2);
        } else {
            input_send_keys(instance->window, ghost_pie_keys, ARRAY_LEN(ghost_pie_keys));
        }
    }

    // Adjust the instance's resolution as needed.
    if (instance_get_id(instance) == active_instance) {
        input_set_sensitivity(compositor->input, config->main_sens);
        instance->alt_res = false;
        wall_resize_instance(instance);
        ninb_set_visible(false);
    }

    // Press the appropriate reset hotkey.
    instance_send_reset_keys(instance);

    // Update the CPU weight for the instance.
    cpu_update_instance(instance, CPU_HIGH);

    if (reset_counter) {
        reset_counter_increment(reset_counter);
    }
    return true;
}

static void
instance_send_reset_keys(struct instance *instance) {
    uint32_t keycode;
    if (instance->state.screen == PREVIEWING) {
        keycode = instance->options.preview_hotkey;
    } else {
        keycode = instance->options.atum_hotkey;
    }
    const struct synthetic_key reset_keys[] = {
        {keycode, true},
        {keycode, false},
    };
    input_send_keys(instance->window, reset_keys, ARRAY_LEN(reset_keys));
}

static void
instance_update_verification(struct instance *instance) {
    ww_assert(instance);
    ww_assert(instance->alive);
    ww_assert(instance->hview_inst && instance->hview_wp);

    // TODO: Make generation more robust for weird sizes

    // Copied from Julti
    int i = 1;
    while (i != instance->options.gui_scale && i < config->stretch_width &&
           i < config->stretch_height && (config->stretch_width / (i + 1)) >= 320 &&
           (config->stretch_height / (i + 1)) >= 240) {
        i++;
    }
    if (instance->options.unicode && i % 2 != 0) {
        i++;
    }
    int square_size = i * 90;
    int extra_height = i * 19;

    // Calculate position on verification output.
    int id = instance_get_id(instance);
    int w = HEADLESS_WIDTH / config->wall_width, h = HEADLESS_HEIGHT / config->wall_height;
    int x = (id % config->wall_width) * w, y = (id / config->wall_width) * h;

    // Whole instance capture
    hview_set_dest(instance->hview_inst, (struct wlr_box){x, y, w, h});

    // Loading square capture
    hview_set_src(instance->hview_wp,
                  (struct wlr_box){.x = 0,
                                   .y = config->stretch_height - (square_size + extra_height),
                                   .width = square_size,
                                   .height = square_size + extra_height});
    hview_set_dest(instance->hview_wp, (struct wlr_box){x, y + h - (square_size + extra_height),
                                                        square_size, square_size + extra_height});
    hview_raise(instance->hview_wp);
}

static void
wall_focus() {
    ww_assert(active_instance != WALL);

    input_focus_window(compositor->input, NULL);
    sleepbg_lock_toggle(false);
    active_instance = WALL;
    for (int i = 0; i < max_instance_id; i++) {
        if (instances[i].alive) {
            render_window_set_enabled(instances[i].window, true);
        }
    }
    render_layer_set_enabled(compositor->render, LAYER_LOCKS, true);
    input_set_on_wall(compositor->input, true);
}

static void
wall_resize_instance(struct instance *instance) {
    ww_assert(instance);
    ww_assert(instance->alive);

    struct wlr_box box = instance_get_wall_box(instance);
    render_window_configure(instance->window, box.x, box.y, config->stretch_width,
                            config->stretch_height);
    render_window_set_dest_size(instance->window, box.width, box.height);
    if (instance->lock_indicator) {
        render_rect_configure(instance->lock_indicator, box);
    }
}

static void
process_bind(struct keybind *keybind, bool held) {
    for (int i = 0; i < keybind->action_count; i++) {
        enum action action = keybind->actions[i];
        bool action_ingame = IS_INGAME_ACTION(action), user_ingame = active_instance != WALL;
        if (!IS_UNIVERSAL_ACTION(action) && action_ingame != user_ingame) {
            continue;
        }
        struct instance *hovered = instance_get_hovered();
        if (hovered) {
            last_held.instance = instance_get_id(hovered);
            last_held.bind = keybind;
        }

        switch (action) {
        case ACTION_ANY_TOGGLE_NINB:
            ninb_set_visible(!ninb_shown);
            break;
        case ACTION_WALL_RESET_ALL:
            if (reset_counter) {
                reset_counter_queue_writes(reset_counter);
            }
            for (int i = 0; i < max_instance_id; i++) {
                if (instances[i].alive && !instances[i].locked) {
                    instance_reset(&instances[i]);
                }
            }
            if (reset_counter) {
                reset_counter_commit_writes(reset_counter);
            }
            break;
        case ACTION_WALL_RESET_ONE:
            if (hovered && !hovered->locked) {
                instance_reset(hovered);
            }
            break;
        case ACTION_WALL_PLAY:
            if (hovered && (hovered->state.screen == INWORLD || hovered->state.screen == TITLE)) {
                instance_play(hovered);
            }
            break;
        case ACTION_WALL_LOCK:
            if (hovered) {
                instance_lock(hovered);
            }
            break;
        case ACTION_WALL_FOCUS_RESET:
            if (hovered && (hovered->state.screen == INWORLD)) {
                bool hovered_id = instance_get_id(hovered);
                if (reset_counter) {
                    reset_counter_queue_writes(reset_counter);
                }
                for (int i = 0; i < max_instance_id; i++) {
                    if (i != hovered_id && instances[i].alive && !instances[i].locked) {
                        instance_reset(&instances[i]);
                    }
                }
                instance_play(hovered);
                if (reset_counter) {
                    reset_counter_commit_writes(reset_counter);
                }
            }
            break;
        case ACTION_INGAME_RESET:
            instance_reset(&instances[active_instance]);
            if (config->wall_bypass) {
                for (int i = 0; i < max_instance_id; i++) {
                    if (i == active_instance) {
                        continue;
                    }
                    struct instance *inst = &instances[i];
                    if (inst->alive && inst->locked && inst->state.screen == INWORLD) {
                        instance_play(inst);
                        return;
                    }
                }
            }
            wall_focus();
            break;
        case ACTION_INGAME_ALT_RES:
            if (!config->has_alt_res) {
                break;
            }
            struct instance *instance = &instances[active_instance];
            if (instance->alt_res) {
                render_window_configure(instance->window, 0, 0, screen_width, screen_height);
                render_window_set_dest_size(instance->window, screen_width, screen_height);
                input_set_sensitivity(compositor->input, config->main_sens);
            } else {
                struct wlr_box box = compute_alt_res();
                render_window_configure(instance->window, box.x, box.y, box.width, box.height);
                render_window_set_dest_size(instance->window, box.width, box.height);
                input_set_sensitivity(compositor->input, config->alt_sens);
            }
            instance->alt_res = !instance->alt_res;
            break;
        }
    }
}

static void
process_state(struct instance *instance) {
    ww_assert(instance->alive);

    char buf[128];
    if (lseek(instance->state_fd, 0, SEEK_SET) == -1) {
        wlr_log_errno(WLR_ERROR, "failed to seek wpstateout");
        return;
    }
    ssize_t len = read(instance->state_fd, buf, 127);
    if (len == 0) {
        return;
    }
    if (len == -1) {
        wlr_log_errno(WLR_ERROR, "failed to read wpstateout");
        return;
    }
    buf[len] = '\0';

    struct state last_state = instance->state;
    if (strcmp(buf, "title") == 0) {
        instance->state.screen = TITLE;
    } else if (strcmp(buf, "waiting") == 0) {
        instance->state.screen = WAITING;
    } else {
        char *a, *b, *ptr;
        for (ptr = buf; *ptr != ','; ptr++) {
            if (ptr - buf == len) {
                wlr_log(WLR_ERROR, "failed to find comma");
                return;
            }
        }
        *ptr = '\0';
        a = buf;
        b = ptr + 1;
        if (strcmp(a, "generating") == 0) {
            instance->state.screen = GENERATING;
            instance->state.data.percent = atoi(b);
        } else if (strcmp(a, "previewing") == 0) {
            if (last_state.screen != PREVIEWING) {
                if (benchmark.running) {
                    // Benchmarking
                    instance_send_reset_keys(instance);
                    if (++benchmark.resets >= BENCHMARK_RESET_COUNT) {
                        benchmark_finish();
                    }
                } else {
                    // Not benchmarking
                    clock_gettime(CLOCK_MONOTONIC, &instance->last_preview);
                    instance_pause(instance);
                }
            }
            instance->state.screen = PREVIEWING;
            instance->state.data.percent = atoi(b);
        } else if (strcmp(a, "inworld") == 0) {
            instance->state.screen = INWORLD;
            if (strcmp(b, "unpaused") == 0) {
                if (last_state.screen == PREVIEWING) {
                    if (instance_get_id(instance) != active_instance) {
                        instance_pause(instance);
                    } else if (config->use_f1) {
                        static const struct synthetic_key f1_keys[] = {
                            {KEY_F1, true},
                            {KEY_F1, false},
                        };
                        input_send_keys(instance->window, f1_keys, ARRAY_LEN(f1_keys));
                    }
                }
                instance->state.data.world = UNPAUSED;
            } else if (strcmp(b, "paused") == 0) {
                instance->state.data.world = PAUSED;
            } else if (strcmp(b, "gamescreenopen") == 0) {
                instance->state.data.world = MENU;
            } else {
                ww_assert(false);
            }
        } else {
            ww_assert(false);
        }
    }
    cpu_update_instance(instance, CPU_NONE);
}

static void
handle_button(struct wl_listener *listener, void *data) {
    struct compositor_button_event *event = data;

    // Ensure the received button is in bounds (a mouse button.)
    int button = event->button - BTN_MOUSE;
    if (button < 0 || button >= (int)ARRAY_LEN(held_buttons)) {
        wlr_log(WLR_INFO, "received button press with unknown button %d", event->button);
        return;
    }

    // Keep track of how many buttons are held as an optimization detail for handle_motion.
    if (event->state != held_buttons[button]) {
        held_buttons_count += event->state ? 1 : -1;
    }
    held_buttons[button] = event->state;

    // Do not process button releases.
    if (!event->state) {
        return;
    }

    for (int i = 0; i < config->bind_count; i++) {
        if (config->binds[i].type != BIND_MOUSE) {
            continue;
        }
        if (config->binds[i].modifiers != held_modifiers) {
            continue;
        }
        if (event->button != config->binds[i].input.button) {
            continue;
        }
        process_bind(&config->binds[i], false);
        break;
    }
    return;
}

static void
handle_configure(struct wl_listener *listener, void *data) {
    struct window_configure_event *event = data;

    if (event->window == ninb_window) {
        ninb_reposition(event->box.width, event->box.height);
    }
}

static void
handle_minimize(struct wl_listener *listener, void *data) {
    struct window_minimize_event *event = data;
    if (event->minimized) {
        ninb_set_visible(false);
    }
}

static bool
handle_key(struct compositor_key_event event) {
    if (!event.state) {
        return false;
    }

    for (int i = 0; i < config->bind_count; i++) {
        if (config->binds[i].type != BIND_KEY) {
            continue;
        }
        if (config->binds[i].modifiers != held_modifiers) {
            continue;
        }

        if (active_instance != WALL && instances[active_instance].state.screen == INWORLD) {
            struct state state = instances[active_instance].state;
            if (state.data.world == PAUSED && !config->binds[i].allow_in_pause) {
                return false;
            }
            if (state.data.world == MENU && !config->binds[i].allow_in_menu) {
                return false;
            }
        }

        for (int j = 0; j < event.nsyms; j++) {
            if (event.syms[j] == config->binds[i].input.sym) {
                process_bind(&config->binds[i], false);
                return true;
            }
        }
    }
    return false;
}

static void
handle_modifiers(struct wl_listener *listener, void *data) {
    xkb_mod_mask_t *modmask = data;
    held_modifiers = *modmask;
}

static void
handle_motion(struct wl_listener *listener, void *data) {
    struct compositor_motion_event *event = data;

    cursor_x = event->x;
    cursor_y = event->y;

    // Do not process mouse motion while ingame.
    if (active_instance != WALL) {
        return;
    }

    // Mouse binds take effect if the user drags the cursor across several instances, but
    // keyboard binds do not.
    if (held_buttons_count == 0) {
        return;
    }
    struct instance *hovered = instance_get_hovered();
    if (!hovered) {
        return;
    }

    for (int i = 0; i < config->bind_count; i++) {
        if (config->binds[i].type != BIND_MOUSE) {
            continue;
        }
        if (config->binds[i].modifiers != held_modifiers) {
            continue;
        }
        if (!held_buttons[config->binds[i].input.button - BTN_MOUSE]) {
            continue;
        }
        bool same_last_instance = last_held.instance == instance_get_id(hovered);
        bool same_last_bind = last_held.bind == &config->binds[i];

        if (!same_last_instance || !same_last_bind) {
            process_bind(&config->binds[i], true);
        }
        return;
    }
}

static void
handle_resize(struct wl_listener *listener, void *data) {
    struct output *output = data;
    int w, h;
    render_output_get_size(output, &w, &h);
    process_resize(w, h);
}

static void
process_resize(int32_t width, int32_t height) {
    wlr_log(WLR_INFO, "handling screen resize of %" PRIi32 " x %" PRIi32, width, height);
    screen_width = width, screen_height = height;
    if (active_instance != WALL) {
        if (instances[active_instance].alt_res && config->has_alt_res) {
            struct wlr_box box = compute_alt_res();
            render_window_configure(instances[active_instance].window, box.x, box.y, box.width,
                                    box.height);
            render_window_set_dest_size(instances[active_instance].window, box.width, box.height);
        } else {
            struct wlr_box box = {
                .x = 0,
                .y = 0,
                .width = width,
                .height = height,
            };
            render_window_configure(instances[active_instance].window, box.x, box.y, box.width,
                                    box.height);
            render_window_set_dest_size(instances[active_instance].window, box.width, box.height);
        }
        if (ninb_window) {
            ninb_reposition(0, 0);
        }
    }

    for (int i = 0; i < max_instance_id; i++) {
        if (i != active_instance && instances[i].alive) {
            wall_resize_instance(&instances[i]);
        }
    }
}

static void
handle_window_unmap(struct wl_listener *listener, void *data) {
    struct window *window = data;

    // Instance death needs to be handled elegantly.
    // If an instance died, handle it. If the instance was focused, instance_handle_death sends
    // the user back to the wall.
    for (int i = 0; i < max_instance_id; i++) {
        if (instances[i].window == window) {
            instance_handle_death(&instances[i]);
        }
    }

    if (window == ninb_window) {
        ninb_window = NULL;
    }
    return;
}

static void
handle_window_map(struct wl_listener *listener, void *data) {
    struct window *window = data;

    struct instance instance = {0};
    const char *name = window_get_name(window);
    if (instance_try_from(window, &instance, inotify_fd)) {
        int id = -1;
        for (int i = 0; i < max_instance_id; i++) {
            if (strcmp(instance.dir, instances[i].dir) == 0) {
                wlr_log(WLR_INFO, "detected instance %d reboot", i);
                id = i;
                break;
            }
        }
        if (id == -1) {
            id = max_instance_id++;
        }
        instances[id] = instance;
        wlr_log(WLR_INFO, "created instance %d (%s)", id, instance.dir);
        wall_resize_instance(&instances[id]);
        instance_update_verification(&instances[id]);
        if (id >= config->wall_width * config->wall_height) {
            wlr_log(WLR_INFO, "more instances than spots on wall - some are invisible");
        }
        render_window_set_layer(window, LAYER_INSTANCE);
        render_window_set_enabled(window, true);
        return;
    } else if (strstr(name, "Ninjabrain Bot")) {
        if (ninb_window) {
            wlr_log(WLR_INFO, "duplicate ninjabrain bot window opened - closing");
            window_close(window);
            return;
        }
        ninb_window = window;
        ninb_set_visible(ninb_shown);
        render_window_set_layer(window, LAYER_FLOATING);
        render_window_set_enabled(window, true);
        return;
    } else {
        if (ninb_window && window_get_pid(window) == window_get_pid(ninb_window)) {
            render_window_set_layer(window, LAYER_FLOATING);
            render_window_set_enabled(window, true);
            return;
        }

        wlr_log(WLR_INFO, "unknown window opened (pid %d, name '%s')", window_get_pid(window),
                name ? name : "unnamed");
        return;
    }
}

static int
handle_signal(int signal_number, void *data) {
    switch (signal_number) {
    case SIGUSR1:
        render_recreate_output(compositor->render);
        wlr_log(WLR_INFO, "recreated wayland output");
        break;
    case SIGUSR2:
        benchmark_toggle();
        break;
    default:
        wlr_log(WLR_INFO, "received signal %d; stopping", signal_number);
        compositor_stop(compositor);
        break;
    };
    return 0;
}

static int
handle_inotify(int fd, uint32_t mask, void *data) {
    char buf[4096] __attribute__((__aligned__(__alignof__(struct inotify_event))));
    const struct inotify_event *event;
    for (;;) {
        ssize_t len = read(fd, &buf, sizeof(buf));
        if (len == -1 && errno != EAGAIN) {
            wlr_log_errno(WLR_ERROR, "read inotify fd");
            return 0;
        }
        if (len <= 0) {
            return 0;
        }
        for (char *ptr = buf; ptr < buf + len; ptr += sizeof(struct inotify_event) + event->len) {
            event = (const struct inotify_event *)ptr;
            if (event->mask & IN_MODIFY) {
                for (int i = 0; i < max_instance_id; i++) {
                    if (instances[i].state_wd == event->wd) {
                        process_state(&instances[i]);
                        break;
                    }
                }
            } else if (event->mask & IN_CREATE) {
                if (config_wd == event->wd && strcmp(event->name, config_filename) == 0) {
                    config_update();
                }
            }
        }
    }
}

static void
print_help(int argc, char **argv) {
    fprintf(stderr, "USAGE: %s [--debug]\n", argc ? argv[0] : "waywall");
}

int
main(int argc, char **argv) {
    enum wlr_log_importance log_level = WLR_INFO;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            if (log_level == WLR_DEBUG) {
                print_help(argc, argv);
                return 1;
            }
            log_level = WLR_DEBUG;
        } else {
            print_help(argc, argv);
            return 1;
        }
    }

    wlr_log_init(log_level, NULL);

    int display_file_fd = open(WAYWALL_DISPLAY_PATH, O_WRONLY | O_CREAT, 0644);
    struct flock lock = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
        .l_pid = getpid(),
    };
    if (fcntl(display_file_fd, F_SETLK, &lock) == -1) {
        wlr_log_errno(WLR_ERROR, "failed to lock waywall-display");
        close(display_file_fd);
        return false;
    }
    ftruncate(display_file_fd, 0);

    config = config_read();
    if (!config) {
        return 1;
    }
    if (config->count_resets) {
        reset_counter = reset_counter_from_file(config->resets_file);
        if (!reset_counter) {
            goto fail_reset_counter;
        }
    }
    if (config->has_cpu) {
        if (!cpu_init()) {
            goto fail_cpu_init;
        }
        if (!cpu_set_group_weight(CPU_IDLE, config->idle_cpu)) {
            goto fail_cpu_init;
        }
        if (!cpu_set_group_weight(CPU_LOW, config->low_cpu)) {
            goto fail_cpu_init;
        }
        if (!cpu_set_group_weight(CPU_HIGH, config->high_cpu)) {
            goto fail_cpu_init;
        }
        if (!cpu_set_group_weight(CPU_ACTIVE, config->active_cpu)) {
            goto fail_cpu_init;
        }
    }

    inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd == -1) {
        wlr_log_errno(WLR_ERROR, "failed to create inotify instance");
        goto fail_inotify_init;
    }
    config_path = config_get_dir();
    if (!config_path) {
        wlr_log(WLR_ERROR, "failed to get config path");
        goto fail_config_dir;
    }
    config_wd = inotify_add_watch(inotify_fd, config_path, IN_CREATE);
    if (config_wd == -1) {
        wlr_log_errno(WLR_ERROR, "failed to watch config directory");
        goto fail_add_watch;
    }
    free(config_path);

    compositor = compositor_create(create_compositor_config());
    ww_assert(compositor);
    event_loop = compositor_get_loop(compositor);
    input_set_sensitivity(compositor->input, config->main_sens);

    struct wl_listener on_button, on_configure, on_minimize, on_modifiers, on_motion, on_resize,
        on_window_map, on_window_unmap;
    on_button.notify = handle_button;
    wl_signal_add(&compositor->input->events.button, &on_button);

    on_configure.notify = handle_configure;
    wl_signal_add(&compositor->render->events.window_configure, &on_configure);

    on_minimize.notify = handle_minimize;
    wl_signal_add(&compositor->render->events.window_minimize, &on_minimize);

    on_modifiers.notify = handle_modifiers;
    wl_signal_add(&compositor->input->events.modifiers, &on_modifiers);

    on_motion.notify = handle_motion;
    wl_signal_add(&compositor->input->events.motion, &on_motion);

    on_resize.notify = handle_resize;
    wl_signal_add(&compositor->render->events.wl_output_resize, &on_resize);

    on_window_map.notify = handle_window_map;
    wl_signal_add(&compositor->render->events.window_map, &on_window_map);

    on_window_unmap.notify = handle_window_unmap;
    wl_signal_add(&compositor->render->events.window_unmap, &on_window_unmap);

    struct wl_event_source *event_sigint =
        wl_event_loop_add_signal(event_loop, SIGINT, handle_signal, NULL);
    struct wl_event_source *event_sigterm =
        wl_event_loop_add_signal(event_loop, SIGTERM, handle_signal, NULL);
    struct wl_event_source *event_sigusr =
        wl_event_loop_add_signal(event_loop, SIGUSR1, handle_signal, NULL);
    struct wl_event_source *event_sigusr2 =
        wl_event_loop_add_signal(event_loop, SIGUSR2, handle_signal, NULL);
    struct wl_event_source *event_inotify =
        wl_event_loop_add_fd(event_loop, inotify_fd, WL_EVENT_READABLE, handle_inotify, NULL);

    input_set_on_wall(compositor->input, true);
    compositor->input->key_callback = handle_key;
    bool success = compositor_run(compositor, display_file_fd);

    if (reset_counter) {
        int count = reset_counter_get_count(reset_counter);
        reset_counter_destroy(reset_counter);
        wlr_log(WLR_INFO, "finished with reset count of %d", count);
    }
    for (int i = 0; i < max_instance_id; i++) {
        if (instances[i].dir) {
            free(instances[i].dir);
        }
    }

    wl_event_source_remove(event_sigint);
    wl_event_source_remove(event_sigterm);
    wl_event_source_remove(event_sigusr);
    wl_event_source_remove(event_sigusr2);
    wl_event_source_remove(event_inotify);
    compositor_destroy(compositor);
    config_destroy(config);
    close(inotify_fd);
    close(display_file_fd);
    remove(WAYWALL_DISPLAY_PATH);
    return success ? 0 : 1;

fail_add_watch:
    free(config_path);

fail_config_dir:
    close(inotify_fd);

fail_inotify_init:
fail_cpu_init:
    if (reset_counter) {
        reset_counter_destroy(reset_counter);
    }

fail_reset_counter:
    config_destroy(config);
    return 1;
}
