#include "compositor.h"
#include "config.h"
#include "instance.h"
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

// TODO: handle extra instances
// TODO: fix verification output
// TODO: improve string handling in options/mod gathering
// TODO: improve error handling (prevent fd leaks, etc) in instance creation
// TODO: free dir paths in instances

// TODO: make config reloading reliable (reads file before written sometimes? also might not handle
// some editors correctly)

#define WALL -1
#define WAYWALL_DISPLAY_PATH "/tmp/waywall-display"

static char *config_path;
static struct config *config;
static struct compositor *compositor;
static struct wl_event_loop *event_loop;
static int inotify_fd;
static int config_wd;

static struct instance instances[128];
static int instance_count;
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

static uint64_t reset_count;
static int reset_count_fd = INT_MIN;

static void config_update();
static struct wlr_box compute_alt_res();
static struct compositor_config create_compositor_config();
static void ninb_reposition(int16_t, int16_t);
static void ninb_set_visible(bool);
static bool prepare_reset_counter();
static void write_reset_count();
static struct instance *instance_get_hovered();
static inline int instance_get_id(struct instance *);
static struct wlr_box instance_get_wall_box(struct instance *);
static void instance_lock(struct instance *);
static void instance_pause(struct instance *);
static void instance_play(struct instance *);
static bool instance_reset(struct instance *);
static void instance_update_verification(struct instance *);
static void wall_focus();
static void wall_resize_instance(struct instance *);
static void process_bind(struct keybind *, bool);
static void process_state(struct instance *);
static bool handle_allow_configure(struct window *, int16_t, int16_t);
static bool handle_button(struct compositor_button_event);
static bool handle_key(struct compositor_key_event);
static void handle_modifiers(uint32_t);
static void handle_motion(struct compositor_motion_event);
static void handle_resize(int32_t, int32_t);
static void handle_window(struct window *, bool);
static int handle_signal(int, void *);
static int handle_inotify(int, uint32_t, void *);

static void
config_update() {
    wlr_log(WLR_INFO, "configuration file was updated");
    struct config *new_config = config_read();
    if (!new_config) {
        return;
    }

    if (new_config->count_resets) {
        ww_assert(new_config->resets_file);

        // TODO: Try handling this? Even though it's annoying
        if (!config->resets_file || strcmp(config->resets_file, new_config->resets_file) != 0) {
            wlr_log(
                WLR_ERROR,
                "updating the reset counter file will not take effect until waywall is restarted");
        }
        if (!config->count_resets) {
            wlr_log(WLR_ERROR,
                    "enabling the reset counter will not take effect until waywall is restarted");
        }
    } else {
        if (reset_count_fd >= 0) {
            wlr_log(WLR_INFO, "disabling reset counting (stopping at %" PRIu64 " resets)",
                    reset_count);
            close(reset_count_fd);
            reset_count_fd = INT_MIN;
        }
    }

    bool same_size = config->cursor_size == new_config->cursor_size;
    bool same_theme = (config->cursor_theme && new_config->cursor_theme)
                          ? strcmp(config->cursor_theme, new_config->cursor_theme) == 0
                          : !config->cursor_theme && !new_config->cursor_theme;
    if (!same_size || !same_theme) {
        // TODO: Figure out how to update cursor theme in xcursor manager
        wlr_log(WLR_ERROR,
                "changing cursor options will not take effect until waywall is restarted");
    }
    config_destroy(config);
    config = new_config;

    // Apply changes from the new configuration.
    compositor_load_config(compositor, create_compositor_config());
    for (unsigned long i = 0; i < ARRAY_LEN(instances); i++) {
        if (instances[i].lock_indicator) {
            compositor_rect_set_color(instances[i].lock_indicator, config->lock_color);
        }
        if (instances[i].alive) {
            instance_update_verification(&instances[i]);
        }
    }
    handle_resize(screen_width, screen_height);
    if (active_instance != WALL && instances[active_instance].alt_res) {
        compositor_set_mouse_sensitivity(compositor, config->alt_sens);
    } else {
        compositor_set_mouse_sensitivity(compositor, config->main_sens);
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
        .confine_pointer = config->confine_pointer,
        .cursor_theme = config->cursor_theme,
        .cursor_size = config->cursor_size,
        .stop_on_close = !config->remain_in_background,
    };
    memcpy(compositor_config.background_color, config->background_color, sizeof(float) * 4);
    return compositor_config;
}

static void
ninb_reposition(int16_t w, int16_t h) {
    ww_assert(ninb_window);

    if (w <= 0 || h <= 0) {
        compositor_window_get_size(ninb_window, &w, &h);
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
        ww_assert(!"unreachable");
        exit(1);
    }
    if (loc == TOP_LEFT || loc == TOP || loc == TOP_RIGHT) {
        y = 0;
    } else if (loc == LEFT || loc == RIGHT) {
        y = (screen_height - size.height) / 2;
    } else if (loc == BOTTOM_LEFT || loc == BOTTOM_RIGHT) {
        y = screen_height - size.height;
    } else {
        ww_assert(!"unreachable");
        exit(1);
    }
    compositor_window_set_dest(ninb_window, (struct wlr_box){x, y, size.width, size.height});
}

static void
ninb_set_visible(bool visible) {
    ninb_shown = visible;
    if (!ninb_window) {
        return;
    }
    ninb_reposition(0, 0);
    compositor_window_set_opacity(ninb_window, visible ? config->ninb_opacity : 0.0f);
    compositor_window_set_top(ninb_window);
}

static bool
prepare_reset_counter() {
    if (!config->count_resets) {
        return true;
    }

    ww_assert(config->resets_file);
    reset_count_fd = open(config->resets_file, O_CREAT | O_RDWR, 0644);
    if (reset_count_fd == -1) {
        wlr_log_errno(WLR_ERROR, "failed to open reset counter");
        return false;
    }
    char buf[64];
    ssize_t len = read(reset_count_fd, buf, STRING_LEN(buf));
    if (len == -1) {
        wlr_log_errno(WLR_ERROR, "failed to read reset counter");
        return false;
    }
    if (len == 0) {
        reset_count = 0;
        return true;
    }
    buf[len] = '\0';
    char *endptr;
    int64_t count = strtoll(buf, &endptr, 10);
    if (endptr == buf) {
        wlr_log(WLR_ERROR, "failed to parse existing reset count ('%s')", buf);
        return false;
    }
    wlr_log(WLR_INFO, "read reset count of %" PRIi64, count);
    reset_count = count;
    return true;
}

static void
write_reset_count() {
    if (reset_count_fd < 0) {
        return;
    }

    if (lseek(reset_count_fd, 0, SEEK_SET) == -1) {
        wlr_log_errno(WLR_ERROR, "failed to seek reset count");
        return;
    }
    char buf[64];
    int written = snprintf(buf, ARRAY_LEN(buf), "%" PRIu64 "\n", reset_count);
    ssize_t n = write(reset_count_fd, buf, written);
    if (n != written) {
        wlr_log_errno(WLR_ERROR, "failed to write reset count");
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
    if (!instances[id].alive || id >= instance_count) {
        return NULL;
    }
    return &instances[id];
}

static inline int
instance_get_id(struct instance *instance) {
    return instance - instances;
}

static struct wlr_box
instance_get_wall_box(struct instance *instance) {
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
instance_pause(struct instance *instance) {
    ww_assert(instance->alive);

    static const struct compositor_key keys[] = {
        {KEY_F3, true},
        {KEY_ESC, true},
        {KEY_ESC, false},
        {KEY_F3, false},
    };
    compositor_send_keys(instance->window, keys, ARRAY_LEN(keys));
}

static void
instance_lock(struct instance *instance) {
    ww_assert(active_instance == WALL);
    ww_assert(instance->alive);

    if (!instance->locked) {
        // Lock the instance.
        instance->locked = true;
        if (!instance->lock_indicator) {
            struct wlr_box box = instance_get_wall_box(instance);
            instance->lock_indicator = compositor_rect_create(compositor, box, config->lock_color);
        }
        compositor_rect_toggle(instance->lock_indicator, instance->locked);
    } else {
        // Unlock the instance.
        ww_assert(instance->lock_indicator);

        switch (config->unlock_behavior) {
        case UNLOCK_ACCEPT:
            instance->locked = false;
            compositor_rect_toggle(instance->lock_indicator, false);
            break;
        case UNLOCK_IGNORE:
            break;
        case UNLOCK_RESET:
            instance->locked = false;
            compositor_rect_toggle(instance->lock_indicator, false);
            instance_reset(instance);
            break;
        }
    }
}

static void
instance_play(struct instance *instance) {
    ww_assert(instance_get_id(instance) != active_instance);
    ww_assert(instance->alive);

    compositor_window_focus(compositor, instance->window);
    compositor_window_configure(instance->window, screen_width, screen_height);
    struct wlr_box box = {
        .x = 0,
        .y = 0,
        .width = screen_width,
        .height = screen_height,
    };
    compositor_window_set_dest(instance->window, box);

    static const struct compositor_key unpause_keys[] = {
        {KEY_ESC, true},
        {KEY_ESC, false},
        {KEY_F1, true},
        {KEY_F1, false},
    };
    compositor_send_keys(instance->window, unpause_keys,
                         config->use_f1 ? ARRAY_LEN(unpause_keys) : ARRAY_LEN(unpause_keys) - 2);

    active_instance = instance_get_id(instance);
    if (instance->locked) {
        ww_assert(instance->lock_indicator);
        compositor_rect_toggle(instance->lock_indicator, false);
        instance->locked = false;
    }

    // We attempt to reread the instance's options file here for any changes. Using inotify to read
    // it when it is updated is unfortunately a bit cumbersome as the game seems to write the file
    // in 512 byte chunks.
    instance_get_options(instance);
}

static bool
instance_reset(struct instance *instance) {
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
        compositor_click(instance->window);
    }

    // If the instance is currently being played, try to fix ghost pie.
    if (instance_get_id(instance) == active_instance && instance->state.screen == INWORLD) {
        static const struct compositor_key ghost_pie_keys[] = {
            {KEY_ESC, true}, {KEY_ESC, false}, {KEY_LEFTSHIFT, false},
            {KEY_F3, true},  {KEY_F3, false},
        };

        if (instance->state.data.world == UNPAUSED) {
            compositor_send_keys(instance->window, ghost_pie_keys + 2,
                                 ARRAY_LEN(ghost_pie_keys) - 2);
        } else {
            compositor_send_keys(instance->window, ghost_pie_keys, ARRAY_LEN(ghost_pie_keys));
        }
    }

    // Adjust the instance's resolution as needed.
    if (instance_get_id(instance) == active_instance) {
        compositor_set_mouse_sensitivity(compositor, config->main_sens);
        instance->alt_res = false;
        wall_resize_instance(instance);
        ninb_set_visible(false);
    }

    // Press the appropriate reset hotkey.
    uint32_t keycode;
    if (instance->state.screen == PREVIEWING) {
        keycode = instance->options.preview_hotkey;
    } else {
        keycode = instance->options.atum_hotkey;
    }
    const struct compositor_key reset_keys[] = {
        {keycode, true},
        {keycode, false},
    };
    compositor_send_keys(instance->window, reset_keys, ARRAY_LEN(reset_keys));

    reset_count++;
    return true;
}

static void
instance_update_verification(struct instance *instance) {
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
    compositor_hview_set_dest(instance->hview_inst, (struct wlr_box){x, y, w, h});

    // Loading square capture
    compositor_hview_set_src(
        instance->hview_wp,
        (struct wlr_box){.x = 0,
                         .y = config->stretch_height - (square_size + extra_height),
                         .width = square_size,
                         .height = square_size + extra_height});
    compositor_hview_set_dest(instance->hview_wp,
                              (struct wlr_box){x, y - h, square_size, square_size + extra_height});
    compositor_hview_set_top(instance->hview_wp);
}

static void
wall_focus() {
    ww_assert(active_instance != WALL);

    compositor_window_focus(compositor, NULL);
    active_instance = WALL;
}

static void
wall_resize_instance(struct instance *instance) {
    ww_assert(instance);

    struct wlr_box box = instance_get_wall_box(instance);
    compositor_window_configure(instance->window, config->stretch_width, config->stretch_height);
    compositor_window_set_dest(instance->window, box);
    if (instance->lock_indicator) {
        compositor_rect_configure(instance->lock_indicator, box);
    }
}

static void
process_bind(struct keybind *keybind, bool held) {
    for (int i = 0; i < keybind->action_count; i++) {
        enum action action = keybind->actions[i];
        bool action_ingame = IS_INGAME_ACTION(action), user_ingame = active_instance != WALL;
        if (action_ingame != user_ingame) {
            continue;
        }
        struct instance *hovered = instance_get_hovered();
        if (hovered) {
            last_held.instance = instance_get_id(hovered);
            last_held.bind = keybind;
        }

        switch (action) {
        case ACTION_WALL_RESET_ALL:
            for (int i = 0; i < instance_count; i++) {
                if (instances[i].alive && !instances[i].locked) {
                    instance_reset(&instances[i]);
                }
            }
            write_reset_count();
            break;
        case ACTION_WALL_RESET_ONE:
            if (hovered && !hovered->locked) {
                if (instance_reset(hovered)) {
                    write_reset_count();
                }
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
                for (int i = 0; i < instance_count; i++) {
                    if (i != hovered_id && instances[i].alive && !instances[i].locked) {
                        instance_reset(&instances[i]);
                    }
                }
                instance_play(hovered);
                write_reset_count();
            }
            break;
        case ACTION_INGAME_RESET:
            instance_reset(&instances[active_instance]);
            if (config->wall_bypass) {
                for (int i = 0; i < instance_count; i++) {
                    if (i == active_instance) {
                        continue;
                    }
                    struct instance *inst = &instances[i];
                    if (inst->alive && inst->locked && inst->state.screen == INWORLD) {
                        instance_play(inst);
                        write_reset_count();
                        return;
                    }
                }
            }
            wall_focus();
            write_reset_count();
            break;
        case ACTION_INGAME_ALT_RES:
            if (!config->has_alt_res) {
                break;
            }
            struct instance *instance = &instances[active_instance];
            if (instance->alt_res) {
                compositor_window_configure(instance->window, screen_width, screen_height);
                compositor_window_set_dest(instance->window,
                                           (struct wlr_box){0, 0, screen_width, screen_height});
                compositor_set_mouse_sensitivity(compositor, config->main_sens);
            } else {
                compositor_window_configure(instance->window, config->alt_width,
                                            config->alt_height);
                compositor_window_set_dest(instance->window, compute_alt_res());
                compositor_set_mouse_sensitivity(compositor, config->alt_sens);
            }
            instance->alt_res = !instance->alt_res;
            break;
        case ACTION_INGAME_TOGGLE_NINB:
            ninb_set_visible(!ninb_shown);
            break;
        }
    }
}

static void
process_state(struct instance *instance) {
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
                clock_gettime(CLOCK_MONOTONIC, &instance->last_preview);
                instance_pause(instance);
            }
            instance->state.screen = PREVIEWING;
            instance->state.data.percent = atoi(b);
        } else if (strcmp(a, "inworld") == 0) {
            instance->state.screen = INWORLD;
            if (strcmp(b, "unpaused") == 0) {
                if (last_state.screen == PREVIEWING &&
                    instance_get_id(instance) != active_instance) {
                    instance_pause(instance);
                }
                instance->state.data.world = UNPAUSED;
            } else if (strcmp(b, "paused") == 0) {
                instance->state.data.world = PAUSED;
            } else if (strcmp(b, "gamescreenopen") == 0) {
                instance->state.data.world = INVENTORY;
            } else {
                ww_assert(false);
            }
        } else {
            ww_assert(false);
        }
    }
}

static bool
handle_allow_configure(struct window *window, int16_t w, int16_t h) {
    if (window != ninb_window) {
        return false;
    }

    ninb_reposition(w, h);
    return true;
}

static bool
handle_button(struct compositor_button_event event) {
    // Ensure the received button is in bounds (a mouse button.)
    int button = event.button - BTN_MOUSE;
    if (button < 0 || button >= (int)ARRAY_LEN(held_buttons)) {
        wlr_log(WLR_INFO, "received button press with unknown button %d", event.button);
        return false;
    }

    // Keep track of how many buttons are held as an optimization detail for handle_motion.
    if (event.state != held_buttons[button]) {
        held_buttons_count += event.state ? 1 : -1;
    }
    held_buttons[button] = event.state;

    // Do not process mouse clicks while ingame and do not process button releases.
    if (active_instance != WALL || !event.state) {
        return false;
    }

    for (int i = 0; i < config->bind_count; i++) {
        if (config->binds[i].type != BIND_MOUSE) {
            continue;
        }
        if (config->binds[i].modifiers != held_modifiers) {
            continue;
        }
        if (event.button != config->binds[i].input.button) {
            continue;
        }
        process_bind(&config->binds[i], false);
        break;
    }
    return true;
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
handle_modifiers(uint32_t modifiers) {
    held_modifiers = modifiers;
}

static void
handle_motion(struct compositor_motion_event event) {
    cursor_x = event.x;
    cursor_y = event.y;

    // Do not process mouse motion while ingame.
    if (active_instance != WALL) {
        return;
    }

    // Mouse binds take effect if the user drags the cursor across several instances, but
    // keyboard binds do not.
    if (held_buttons_count == 0) {
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
        bool same_last_instance = last_held.instance == instance_get_id(instance_get_hovered());
        bool same_last_bind = last_held.bind == &config->binds[i];

        if (!same_last_instance || !same_last_bind) {
            process_bind(&config->binds[i], true);
        }
        return;
    }
}

static void
handle_resize(int32_t width, int32_t height) {
    wlr_log(WLR_INFO, "handling screen resize of %" PRIi32 " x %" PRIi32, width, height);
    screen_width = width, screen_height = height;
    if (active_instance != WALL) {
        if (instances[active_instance].alt_res && config->has_alt_res) {
            compositor_window_configure(instances[active_instance].window, config->alt_width,
                                        config->alt_height);
            compositor_window_set_dest(instances[active_instance].window, compute_alt_res());
        } else {
            struct wlr_box box = {
                .x = 0,
                .y = 0,
                .width = width,
                .height = height,
            };
            compositor_window_configure(instances[active_instance].window, width, height);
            compositor_window_set_dest(instances[active_instance].window, box);
        }
        if (ninb_window) {
            ninb_reposition(0, 0);
        }
    }

    for (int i = 0; i < instance_count; i++) {
        if (i != active_instance) {
            wall_resize_instance(&instances[i]);
        }
    }
}

static void
handle_window(struct window *window, bool map) {
    // Instance death needs to be handled elegantly.
    if (!map) {
        if (window == ninb_window) {
            ninb_window = NULL;
        }
        for (int i = 0; i < instance_count; i++) {
            if (instances[i].window == window) {
                wlr_log(WLR_ERROR, "instance %d died", i);
                instances[i].alive = false;
                instances[i].window = NULL;
                return;
            }
        }
        return;
    }

    struct instance instance = {0};
    const char *name = compositor_window_get_name(window);
    if (instance_try_from(window, &instance, inotify_fd)) {
        // TODO: Check to see if this instance has the same properties of a dead instance and
        // replace it
        int id = instance_count;
        instances[instance_count++] = instance;
        wlr_log(WLR_INFO, "created instance %d (%s)", instance_count, instance.dir);
        wall_resize_instance(&instances[id]);
        instance_update_verification(&instances[id]);
        return;
    } else if (strstr(name, "Ninjabrain Bot")) {
        if (ninb_window) {
            wlr_log(WLR_INFO, "duplicate ninjabrain bot window opened - closing");
            compositor_window_close(window);
            return;
        }
        ninb_window = window;
        ninb_set_visible(ninb_shown);
        return;
    } else {
        compositor_window_set_opacity(window, 0.0f);

        char procbuf[PATH_MAX], linkbuf[PATH_MAX];
        snprintf(procbuf, ARRAY_LEN(procbuf), "/proc/%d/exe", compositor_window_get_pid(window));
        ssize_t len = readlink(procbuf, linkbuf, ARRAY_LEN(linkbuf) - 1);
        if (len == -1) {
            wlr_log_errno(WLR_ERROR, "failed to read executable of process");
            compositor_window_close(window);
            return;
        }
        linkbuf[len] = '\0';
        char *name = strrchr(linkbuf, '/');
        if (!name || strncmp(name, "/java", STRING_LEN("/java")) != 0) {
            wlr_log(WLR_INFO, "closing unknown window '%s' (exe: '%s')", name ? name : "unnamed",
                    linkbuf);
            compositor_window_close(window);
        }
        return;
    }
}

static int
handle_signal(int signal_number, void *data) {
    switch (signal_number) {
    case SIGUSR1:
        if (compositor_recreate_output(compositor)) {
            wlr_log(WLR_INFO, "recreated wayland output");
        }
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
                for (int i = 0; i < instance_count; i++) {
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

int
main() {
    // TODO: add WLR_DEBUG flag
    wlr_log_init(WLR_INFO, NULL);

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
    if (!prepare_reset_counter()) {
        return 1;
    }

    inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd == -1) {
        wlr_log_errno(WLR_ERROR, "failed to create inotify instance");
        return 1;
    }
    config_path = config_get_dir();
    if (!config_path) {
        wlr_log(WLR_ERROR, "failed to get config path");
        return 1;
    }
    config_wd = inotify_add_watch(inotify_fd, config_path, IN_CREATE);
    if (config_wd == -1) {
        wlr_log_errno(WLR_ERROR, "failed to watch config directory");
        return 1;
    }
    free(config_path);

    struct compositor_vtable vtable = {
        .allow_configure = handle_allow_configure,
        .button = handle_button,
        .key = handle_key,
        .modifiers = handle_modifiers,
        .motion = handle_motion,
        .resize = handle_resize,
        .window = handle_window,
    };
    compositor = compositor_create(vtable, create_compositor_config());
    ww_assert(compositor);
    event_loop = compositor_get_loop(compositor);
    compositor_set_mouse_sensitivity(compositor, config->main_sens);

    struct wl_event_source *event_sigint =
        wl_event_loop_add_signal(event_loop, SIGINT, handle_signal, NULL);
    struct wl_event_source *event_sigterm =
        wl_event_loop_add_signal(event_loop, SIGTERM, handle_signal, NULL);
    struct wl_event_source *event_sigusr =
        wl_event_loop_add_signal(event_loop, SIGUSR1, handle_signal, NULL);
    struct wl_event_source *event_inotify =
        wl_event_loop_add_fd(event_loop, inotify_fd, WL_EVENT_READABLE, handle_inotify, NULL);

    compositor_run(compositor, display_file_fd);

    if (reset_count_fd > 0) {
        write_reset_count();
        wlr_log(WLR_INFO, "finished with reset count of %" PRIu64, reset_count);
    }

    wl_event_source_remove(event_sigint);
    wl_event_source_remove(event_sigterm);
    wl_event_source_remove(event_sigusr);
    wl_event_source_remove(event_inotify);
    compositor_destroy(compositor);
    close(inotify_fd);
    close(display_file_fd);
    remove(WAYWALL_DISPLAY_PATH);
    return 0;
}
