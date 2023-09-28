#include "compositor.h"
#include "config.h"
#include "util.h"
#include <dirent.h>
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
#include <zip.h>

// TODO: handle ninjabrain bot
// TODO: handle remote window resizing
// TODO: handle fullscreen
// TODO: handle remote window aspect ratio that is incompatible with wall aspect ratio

#define WALL -1

struct state {
    enum {
        TITLE,
        WAITING,
        GENERATING,
        PREVIEWING,
        INWORLD,
    } screen;
    union {
        int percent;
        enum {
            UNPAUSED,
            PAUSED,
            INVENTORY,
        } world;
    } data;
};

static struct config *config;
static struct compositor *compositor;
static struct wl_event_loop *event_loop;
static int inotify_fd;

static struct instance {
    struct window *window;
    const char *dir;
    int wd;
    int fd;
    struct state state;

    bool alive, locked;
    bool has_stateout, has_wp;
    struct {
        uint8_t atum;
        uint8_t preview;
        uint8_t fullscreen;
    } hotkeys;
} instances[128];
static int instance_count;
static int active_instance = WALL;

static int cursor_x, cursor_y;
static uint32_t held_modifiers;
static bool held_buttons[8]; // NOTE: Button count is a bit low here, but this supports all current
                             // mouse buttons.
static int held_buttons_count;

static struct instance *instance_get_hovered();
static inline int instance_get_id(struct instance *);
static bool instance_get_info(struct instance *);
static void instance_pause(struct instance *);
static void instance_play(struct instance *);
static bool instance_reset(struct instance *);
static void wall_focus();
static void process_bind(struct keybind *, bool);
static void process_state(struct instance *);
static bool handle_button(struct compositor_button_event);
static bool handle_key(struct compositor_key_event);
static void handle_modifiers(uint32_t);
static void handle_motion(struct compositor_motion_event);
static bool handle_window(struct window *, bool);
static int handle_signal(int, void *);
static int handle_inotify(int, uint32_t, void *);

static struct instance *
instance_get_hovered() {
    if (active_instance != WALL) {
        return NULL;
    }

    int32_t screen_width, screen_height;
    compositor_get_screen_size(compositor, &screen_width, &screen_height);
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

static bool
instance_get_info(struct instance *instance) {
    // Open the instance's mods directory.
    char buf[PATH_MAX];
    strncpy(buf, instance->dir, PATH_MAX);
    static const char mods[] = "/mods/";
    size_t limit = PATH_MAX - strlen(buf) - 1;
    if (limit < STRING_LEN(mods)) {
        wlr_log(WLR_ERROR, "instance path too long");
        return false;
    }
    strcat(buf, mods);
    limit += STRING_LEN(mods);
    DIR *dir = opendir(buf);
    if (!dir) {
        wlr_log_errno(WLR_ERROR, "failed to open mod directory");
        return false;
    }

    // Check to see which relevant mods the instance has.
    struct dirent *dirent;
    bool has_atum = false;
    while ((dirent = readdir(dir))) {
        // Skip the '.' and '..' entries, as well as files which are not enabled mods.
        if (dirent->d_name[0] == '.') {
            continue;
        }
        const char *ext = strrchr(dirent->d_name, '.');
        if (!ext || strcmp(ext, ".jar") != 0) {
            continue;
        }
        char buf_file[PATH_MAX];
        strncpy(buf_file, buf, PATH_MAX);
        if (limit < strlen(dirent->d_name)) {
            wlr_log(WLR_ERROR, "failed to open mod '%s': path too long", buf_file);
            continue;
        }
        strcat(buf_file, dirent->d_name);

        // Scan the mod jar for any files/folders that indicate what mod it is.
        int err;
        struct zip *zip = zip_open(buf_file, ZIP_RDONLY, &err);
        if (!zip) {
            zip_error_t error;
            zip_error_init_with_code(&error, err);
            wlr_log(WLR_ERROR, "failed to open mod '%s': %s", buf_file, zip_error_strerror(&error));
            zip_error_fini(&error);
            continue;
        }
        for (int64_t i = 0; i < zip_get_num_entries(zip, 0); i++) {
            struct zip_stat stat;
            if (zip_stat_index(zip, i, 0, &stat) == -1) {
                zip_error_t *error = zip_get_error(zip);
                wlr_log(WLR_ERROR, "failed to stat entry %" PRIi64 ", of '%s': %s", i, buf_file,
                        zip_error_strerror(error));
                zip_close(zip);
                closedir(dir);
                return false;
            }

            // NOTE: This is a bit of a weird method, but I think it's better than a list of hashes
            // or annoying version comparisons (and JSON parsing.)
            if (strcmp(stat.name, "me/voidxwalker/autoreset/") == 0) {
                has_atum = true;
                break;
            } else if (strcmp(stat.name, "me/voidxwalker/worldpreview/") == 0) {
                instance->has_wp = true;
            } else if (strcmp(stat.name, "me/voidxwalker/worldpreview/StateOutputHelper.class") ==
                       0) {
                instance->has_wp = instance->has_stateout = true;
                break;
            } else if (strcmp(stat.name, "xyz/tildejustin/stateoutput/") == 0) {
                instance->has_stateout = true;
                break;
            }
        }
        zip_close(zip);
    }
    if (!has_atum) {
        wlr_log(WLR_ERROR, "instance at '%s' has no atum", buf);
        closedir(dir);
        return false;
    }
    closedir(dir);

    // Check for the user's Atum, WorldPreview, and fullscreen hotkeys.
    strncpy(buf, instance->dir, PATH_MAX);
    const char options[] = "/options.txt";
    limit = PATH_MAX - strlen(buf) - 1;
    if (limit < STRING_LEN(options)) {
        wlr_log(WLR_ERROR, "instance path too long");
        return false;
    }
    strcat(buf, options);
    FILE *file = fopen(buf, "r");
    if (!file) {
        wlr_log_errno(WLR_ERROR, "failed to open options file at '%s'", buf);
        return false;
    }
    char line[256];
    bool atum_hotkey = false, wp_hotkey = false, fullscreen_hotkey = false;
    while (fgets(line, STRING_LEN(line), file)) {
        const char atum[] = "key_Create New World:";
        const char wp[] = "key_Leave Preview:";
        const char fullscreen[] = "key_key.fullscreen:";

        uint8_t *keycode;
        char *end = strrchr(line, '\n');
        if (end) {
            *end = '\0';
        }
        if (strncmp(line, atum, STRING_LEN(atum)) == 0) {
            const char *key = line + STRING_LEN(atum);
            atum_hotkey = true;
            if ((keycode = get_minecraft_keycode(key))) {
                instance->hotkeys.atum = *keycode;
            } else {
                wlr_log(WLR_INFO, "unknown atum hotkey '%s' in '%s': setting to default of F6", key,
                        buf);
                instance->hotkeys.atum = KEY_F6 + 8;
            }
        } else if (strncmp(line, wp, STRING_LEN(wp)) == 0) {
            const char *key = line + STRING_LEN(wp);
            wp_hotkey = true;
            if ((keycode = get_minecraft_keycode(key))) {
                instance->hotkeys.preview = *keycode;
            } else {
                wlr_log(WLR_INFO,
                        "unknown leave preview hotkey '%s' in '%s': setting to default of H", key,
                        buf);
                instance->hotkeys.preview = KEY_H + 8;
            }
        } else if (strncmp(line, fullscreen, STRING_LEN(fullscreen)) == 0) {
            const char *key = line + STRING_LEN(fullscreen);
            fullscreen_hotkey = true;
            if ((keycode = get_minecraft_keycode(key))) {
                instance->hotkeys.fullscreen = *keycode;
            } else {
                wlr_log(WLR_INFO,
                        "unknown fullscreen hotkey '%s' in '%s': setting to default of F11", key,
                        buf);
                instance->hotkeys.fullscreen = KEY_F11 + 8;
            }
        }
    }

    if (!atum_hotkey) {
        wlr_log(WLR_INFO, "no atum hotkey found in '%s': setting to default of F6", buf);
        instance->hotkeys.atum = KEY_F6 + 8;
    }
    if (!wp_hotkey) {
        wlr_log(WLR_INFO, "no leave preview hotkey found in '%s': setting to default of H", buf);
        instance->hotkeys.preview = KEY_H + 8;
    }
    if (!fullscreen_hotkey) {
        wlr_log(WLR_INFO, "no fullscreen hotkey found in '%s': setting to default of F11", buf);
        instance->hotkeys.fullscreen = KEY_F11 + 8;
    }

    return true;
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
instance_play(struct instance *instance) {
    ww_assert(instance_get_id(instance) != active_instance);
    ww_assert(instance->alive);

    int32_t screen_width, screen_height;
    compositor_get_screen_size(compositor, &screen_width, &screen_height);
    compositor_focus_window(compositor, instance->window);
    compositor_configure_window(instance->window, 0, 0, screen_width, screen_height);

    static const struct compositor_key unpause_keys[] = {
        {KEY_ESC, true},
        {KEY_ESC, false},
        {KEY_F1, true},
        {KEY_F1, false},
    };
    compositor_send_keys(instance->window, unpause_keys,
                         config->use_f1 ? ARRAY_LEN(unpause_keys) : ARRAY_LEN(unpause_keys) - 2);

    active_instance = instance_get_id(instance);
}

static bool
instance_reset(struct instance *instance) {
    ww_assert(instance->alive);

    // Do not allow resets on the dirt screen.
    if (instance->state.screen == GENERATING || instance->state.screen == WAITING) {
        return false;
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

    // Adjust the instance's resolution as needed. TODO: Fullscreen
    if (instance_get_id(instance) == active_instance) {
        int id = instance_get_id(instance);
        compositor_configure_window(instance->window,
                                    config->stretch_width * (id % config->wall_width),
                                    config->stretch_height * (id / config->wall_width),
                                    config->stretch_width, config->stretch_height);
    }

    // Press the appropriate reset hotkey.
    uint32_t keycode;
    if (instance->state.screen == PREVIEWING) {
        keycode = instance->hotkeys.preview;
    } else {
        keycode = instance->hotkeys.atum;
    }
    const struct compositor_key reset_keys[] = {
        {keycode, true},
        {keycode, false},
    };
    compositor_send_keys(instance->window, reset_keys, ARRAY_LEN(reset_keys));

    return true;
}

static void
wall_focus() {
    compositor_focus_window(compositor, NULL);
    active_instance = WALL;
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

        switch (action) {
        case ACTION_WALL_RESET_ALL:
            for (int i = 0; i < instance_count; i++) {
                if (instances[i].alive && !instances[i].locked) {
                    instance_reset(&instances[i]);
                }
            }
            break;
        case ACTION_WALL_RESET_ONE:
            if (hovered && !hovered->locked) {
                instance_reset(hovered);
            }
            break;
        case ACTION_WALL_PLAY:
            if (hovered) {
                instance_play(hovered);
            }
            break;
        case ACTION_WALL_LOCK:
            if (hovered) {
                // TODO: show lock status
                hovered->locked = !hovered->locked;
            }
            break;
        case ACTION_WALL_FOCUS_RESET:
            if (hovered) {
                bool hovered_id = instance_get_id(hovered);
                for (int i = 0; i < instance_count; i++) {
                    if (i != hovered_id && instances[i].alive && instances[i].locked) {
                        instance_reset(&instances[i]);
                    }
                    instance_play(hovered);
                }
            }
            break;
        case ACTION_INGAME_RESET:
            instance_reset(&instances[active_instance]);
            wall_focus();
            break;
        }
    }
}

static void
process_state(struct instance *instance) {
    char buf[128];
    if (lseek(instance->fd, 0, SEEK_SET) == -1) {
        wlr_log_errno(WLR_ERROR, "failed to seek wpstateout");
        return;
    }
    ssize_t len = read(instance->fd, buf, 127);
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

    // Mouse binds take effect if the user drags the cursor across several instances, but keyboard
    // binds do not.
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
        process_bind(&config->binds[i], true);
        return;
    }
}

static bool
handle_window(struct window *window, bool map) {
    // TODO: some way to kill the window if we don't want it

    // Instance death needs to be handled elegantly.
    if (!map) {
        for (int i = 0; i < instance_count; i++) {
            if (instances[i].window == window) {
                // TODO: handle instance death and reboot
                wlr_log(WLR_ERROR, "instance %d died", i);
                instances[i].alive = false;
                instances[i].window = NULL;
                return false;
            }
        }
        return false;
    }

    // Find the instance's directory.
    pid_t pid = compositor_get_window_pid(window);
    char buf[PATH_MAX], dir_path[PATH_MAX];
    if (snprintf(buf, PATH_MAX, "/proc/%d/cwd", (int)pid) >= PATH_MAX) {
        wlr_log(WLR_ERROR, "tried to readlink of path longer than 512 bytes");
        return false;
    }
    ssize_t len = readlink(buf, dir_path, PATH_MAX - 1);
    dir_path[len] = '\0';

    // Check that the instance has the relevant mods and hotkeys.
    struct instance instance;
    instance.alive = true;
    instance.dir = strdup(dir_path);
    if (!instance_get_info(&instance)) {
        free((void *)instance.dir);
        return false;
    }

    // Open the correct file for reading the instance's state.
    static const char stateout[] = "/wpstateout.txt";
    static const char log[] = "/logs/latest.log";
    const char *state_name = instance.has_stateout ? stateout : log;
    size_t limit = PATH_MAX - len - 1;
    if (limit < strlen(state_name)) {
        wlr_log(WLR_ERROR, "instance path too long");
        return false;
    }
    strcat(dir_path, state_name);
    instance.fd = open(dir_path, O_CLOEXEC | O_NONBLOCK | O_RDONLY);
    if (instance.fd == -1) {
        wlr_log_errno(WLR_ERROR, "failed to open instance state file (%s)", dir_path);
        return false;
    }
    instance.wd = inotify_add_watch(inotify_fd, dir_path, IN_MODIFY);
    if (instance.wd == -1) {
        wlr_log_errno(WLR_ERROR, "failed to add instance state file (%s) to inotify", dir_path);
        return false;
    }
    instance.window = window;
    instance.state.screen = TITLE;

    int id = instance_count;
    instances[instance_count++] = instance;
    wlr_log(WLR_INFO, "created instance %d (%s)", instance_count, instance.dir);
    compositor_configure_window(window, config->stretch_width * (id % config->wall_width),
                                config->stretch_height * (id / config->wall_width),
                                config->stretch_width, config->stretch_height);
    return false;
}

static int
handle_signal(int signal_number, void *data) {
    wlr_log(WLR_INFO, "received signal %d; stopping", signal_number);
    compositor_stop(compositor);
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
                    if (instances[i].wd == event->wd) {
                        process_state(&instances[i]);
                    }
                }
            }
        }
    }
}

int
main() {
    // TODO: add WLR_DEBUG flag
    wlr_log_init(WLR_INFO, NULL);

    config = config_read();
    if (!config) {
        return 1;
    }

    inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd == -1) {
        wlr_log_errno(WLR_ERROR, "failed to create inotify instance");
        return 1;
    }

    struct compositor_vtable vtable = {
        .button = handle_button,
        .key = handle_key,
        .modifiers = handle_modifiers,
        .motion = handle_motion,
        .window = handle_window,
    };
    compositor = compositor_create(vtable);
    ww_assert(compositor);
    event_loop = compositor_get_loop(compositor);

    struct wl_event_source *event_sigint =
        wl_event_loop_add_signal(event_loop, SIGINT, handle_signal, NULL);
    struct wl_event_source *event_sigterm =
        wl_event_loop_add_signal(event_loop, SIGTERM, handle_signal, NULL);
    struct wl_event_source *event_inotify =
        wl_event_loop_add_fd(event_loop, inotify_fd, WL_EVENT_READABLE, handle_inotify, NULL);

    compositor_run(compositor);

    wl_event_source_remove(event_sigint);
    wl_event_source_remove(event_sigterm);
    wl_event_source_remove(event_inotify);
    compositor_destroy(compositor);
    close(inotify_fd);
    return 0;
}
