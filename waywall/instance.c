#include "instance.h"
#include "compositor.h"
#include "config.h"
#include "str.h"
#include "util.h"
#include "wall.h"
#include "waywall.h"
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include <zip.h>

// TODO: only use state file for wpstateout, keep log file open anyway for other things
// TODO: get rid of str.h because it's silly and is basically only used for nicer string appending

static struct {
    const char *name;
    uint8_t code;
} mc_keycodes[] = {
    {"0", KEY_0},     {"1", KEY_1},     {"2", KEY_2},     {"3", KEY_3},   {"4", KEY_4},
    {"5", KEY_5},     {"6", KEY_6},     {"7", KEY_7},     {"8", KEY_8},   {"9", KEY_9},
    {"a", KEY_A},     {"b", KEY_B},     {"c", KEY_C},     {"d", KEY_D},   {"e", KEY_E},
    {"f", KEY_F},     {"g", KEY_G},     {"h", KEY_H},     {"i", KEY_I},   {"j", KEY_J},
    {"k", KEY_K},     {"l", KEY_L},     {"m", KEY_M},     {"n", KEY_N},   {"o", KEY_O},
    {"p", KEY_P},     {"q", KEY_Q},     {"r", KEY_R},     {"s", KEY_S},   {"t", KEY_T},
    {"u", KEY_U},     {"v", KEY_V},     {"w", KEY_W},     {"x", KEY_X},   {"y", KEY_Y},
    {"z", KEY_Z},     {"f1", KEY_F1},   {"f2", KEY_F2},   {"f3", KEY_F3}, {"f4", KEY_F4},
    {"f5", KEY_F5},   {"f6", KEY_F6},   {"f7", KEY_F7},   {"f8", KEY_F8}, {"f9", KEY_F9},
    {"f10", KEY_F10}, {"f11", KEY_F11}, {"f12", KEY_F12},
};

static inline uint8_t *
get_mc_keycode(const char *name) {
    static const char key_prefix[] = "key.keyboard.";
    if (strlen(name) <= STRING_LEN(key_prefix)) {
        wlr_log(WLR_ERROR, "tried reading minecraft keycode with invalid prefix");
        return NULL;
    }

    for (unsigned long i = 0; i < ARRAY_LEN(mc_keycodes); i++) {
        if (strcmp(name + STRING_LEN(key_prefix), mc_keycodes[i].name) == 0) {
            return &mc_keycodes[i].code;
        }
    }
    return NULL;
}

static bool
check_instance_dirs(const char *path) {
    static const char *dir_names[] = {"config", "logs", "mods", "saves"};

    DIR *dir = opendir(path);
    if (!dir) {
        wlr_log_errno(WLR_ERROR, "failed to open instance directory ('%s')", path);
        return false;
    }

    struct dirent *dirent;
    size_t count = 0;
    while ((dirent = readdir(dir))) {
        for (size_t i = 0; i < ARRAY_LEN(dir_names); i++) {
            if (strcmp(dirent->d_name, dir_names[i]) == 0) {
                count++;
                break;
            }
        }
    }
    closedir(dir);
    return count == ARRAY_LEN(dir_names);
}

static bool
get_mods(struct instance *instance) {
    char dirbuf[PATH_MAX];
    struct str mods_dir = str_new(dirbuf, PATH_MAX);
    mods_dir = str_copy(mods_dir, str_from(instance->dir));
    mods_dir = str_appendl(mods_dir, "/mods/");

    DIR *dir = opendir(str_into(mods_dir));
    if (!dir) {
        wlr_log_errno(WLR_ERROR, "failed to open mods directory ('%s')", str_into(mods_dir));
        return false;
    }

    struct dirent *dirent;
    while ((dirent = readdir(dir))) {
        if (dirent->d_name[0] == '.') {
            continue;
        }
        const char *ext = strrchr(dirent->d_name, '.');
        if (!ext || strcmp(ext, ".jar") != 0) {
            continue;
        }

        char modbuf[PATH_MAX];
        struct str mod_path = str_new(modbuf, PATH_MAX);
        mod_path = str_copy(mod_path, mods_dir);
        mod_path = str_append(mod_path, str_from(dirent->d_name));

        int err;
        struct zip *zip = zip_open(str_into(mod_path), ZIP_RDONLY, &err);
        if (!zip) {
            zip_error_t error;
            zip_error_init_with_code(&error, err);
            wlr_log(WLR_ERROR, "failed to open mod zip at '%s': %s", str_into(mod_path),
                    zip_error_strerror(&error));
            zip_error_fini(&error);
            continue;
        }

        for (int64_t i = 0; i < zip_get_num_entries(zip, 0); i++) {
            struct zip_stat stat;
            if (zip_stat_index(zip, i, 0, &stat) == -1) {
                zip_error_t *error = zip_get_error(zip);
                wlr_log(WLR_ERROR, "failed to stat entry %" PRIi64 " of '%s': %s", i,
                        str_into(mod_path), zip_error_strerror(error));
                goto fail_stat;
            }

            static const char atum[] = "me/voidxwalker/autoreset/";
            static const char ranked[] = "com/mcsr/projectelo/";
            static const char standard_settings[] = "com/kingcontaria/standardsettings/";
            static const char state_output[] = "xyz/tildejustin/stateoutput/";
            static const char world_preview[] = "me/voidxwalker/worldpreview/";
            static const char world_preview_stateoutput[] =
                "me/voidxwalker/worldpreview/StateOutputHelper.class";

            if (strcmp(stat.name, atum) == 0) {
                instance->mods.atum = true;
                break;
            }
            if (strcmp(stat.name, ranked) == 0) {
                instance->mods.ranked = true;
                break;
            }
            if (strcmp(stat.name, standard_settings) == 0) {
                instance->mods.standard_settings = true;
                break;
            }
            if (strcmp(stat.name, state_output) == 0) {
                instance->mods.state_output = true;
                break;
            }
            if (strcmp(stat.name, world_preview) == 0) {
                instance->mods.world_preview = true;
                continue;
            }
            if (strcmp(stat.name, world_preview_stateoutput) == 0) {
                instance->mods.world_preview = instance->mods.state_output = true;
                break;
            }
        }

        zip_close(zip);
        continue;

    fail_stat:
        zip_close(zip);
        closedir(dir);
        return false;
    } // while ((dirent = readdir(dir)))

    closedir(dir);
    if (!instance->mods.atum) {
        wlr_log(WLR_INFO, "no atum found in '%s'", str_into(mods_dir));
    }
    return true;
}

static bool
get_version(struct instance *instance) {
    // TODO: Make more robust. Maybe support snapshots, Sodium version hiding?
    const char *title = window_get_name(instance->window);
    int version[3] = {0};

    int n = sscanf(title, "Minecraft %2d.%2d.%2d", &version[0], &version[1], &version[2]);
    if (n == 3) {
        instance->version = version[1];
        return true;
    }

    n = sscanf(title, "Minecraft* %2d.%2d.%2d", &version[0], &version[1], &version[2]);
    if (n == 3) {
        instance->version = version[1];
        return true;
    }

    wlr_log(WLR_ERROR, "failed to parse Minecraft version: '%s'", title);
    return false;
}

static void
open_state_file(struct instance *instance) {
    char buf[PATH_MAX];
    struct str path = str_new(buf, PATH_MAX);
    path = str_copy(path, str_from(instance->dir));
    if (instance->mods.state_output) {
        path = str_appendl(path, "/wpstateout.txt");
    } else {
        path = str_appendl(path, "/logs/latest.log");
    }

    instance->state_fd = open(str_into(path), O_RDONLY);
    if (instance->state_fd == -1) {
        wlr_log_errno(WLR_ERROR, "failed to open state file ('%s')", str_into(path));
        return;
    }
    instance->state_wd =
        inotify_add_watch(g_inotify, str_into(path), IN_MODIFY | IN_DELETE_SELF | IN_MOVE_SELF);
    if (instance->state_wd == -1) {
        wlr_log_errno(WLR_ERROR, "failed to watch state file ('%s')", str_into(path));
        close(instance->state_fd);
        instance->state_fd = -1;
        return;
    }
}

static void
pause_instance(struct instance *instance) {
    static const struct synthetic_key keys[] = {
        {KEY_F3, true},
        {KEY_ESC, true},
        {KEY_ESC, false},
        {KEY_F3, false},
    };
    input_send_keys(instance->window, keys, ARRAY_LEN(keys));
}

static void
process_log_update(struct instance *instance) {
    // TODO
    ww_assert(false);
}

static void
process_state_update(struct instance *instance) {
    ww_assert(instance->state_fd > 0);
    char buf[128];

    if (lseek(instance->state_fd, 0, SEEK_SET) == -1) {
        wlr_log_errno(WLR_ERROR, "failed to seek wpstateout.txt");
        return;
    }
    ssize_t n = read(instance->state_fd, buf, ARRAY_LEN(buf) - 1);
    if (n == 0) {
        return;
    } else if (n == -1) {
        wlr_log_errno(WLR_ERROR, "failed to read wpstateout.txt");
        return;
    }
    buf[n] = '\0';

    struct state prev = instance->state;
    if (strcmp(buf, "title") == 0) {
        instance->state.screen = TITLE;
    } else if (strcmp(buf, "waiting") == 0) {
        instance->state.screen = WAITING;
        instance->reset_wait = false;
    } else {
        char *split = strchr(buf, ',');
        if (!split) {
            wlr_log(WLR_ERROR, "failed to find comma in wpstateout.txt ('%s')", buf);
            return;
        }
        *split = '\0';

        char *a = buf, *b = split + 1;
        if (strcmp(a, "generating") == 0) {
            instance->reset_wait = false;
            instance->state.screen = GENERATING;
            instance->state.data.percent = atoi(b);
        } else if (strcmp(a, "previewing") == 0) {
            instance->reset_wait = false;
            instance->state.screen = PREVIEWING;
            instance->state.data.percent = atoi(b);

            if (prev.screen != PREVIEWING) {
                clock_gettime(CLOCK_MONOTONIC, &instance->last_preview);
                pause_instance(instance); // F3+Esc to hide the menu buttons
            }
        } else if (strcmp(a, "inworld") == 0) {
            instance->state.screen = INWORLD;

            if (strcmp(b, "unpaused") == 0) {
                instance->state.data.inworld = UNPAUSED;

                if (prev.screen == PREVIEWING) {
                    if (g_wall->active_instance == instance->id) {
                        if (g_config->use_f1) {
                            static const struct synthetic_key f1_keys[] = {
                                {KEY_F1, true},
                                {KEY_F1, false},
                            };
                            input_send_keys(instance->window, f1_keys, ARRAY_LEN(f1_keys));
                        }
                    } else {
                        pause_instance(instance);
                    }
                }
            } else if (strcmp(b, "paused") == 0) {
                instance->state.data.inworld = PAUSED;
            } else if (strcmp(b, "gamescreenopen") == 0) {
                instance->state.data.inworld = MENU;
            } else {
                wlr_log(WLR_ERROR, "failed to read wpstateout.txt ('%s')", buf);
                return;
            }
        } else {
            wlr_log(WLR_ERROR, "failed to read wpstateout.txt ('%s')", buf);
            return;
        }
    }
}

void
instance_destroy(struct instance *instance) {
    if (instance->dir) {
        free(instance->dir);
        instance->dir = NULL;
    }

    if (instance->state_fd > 0) {
        close(instance->state_fd);
        instance->state_fd = -1;
    }
    if (instance->state_wd > 0) {
        if (inotify_rm_watch(g_inotify, instance->state_wd) == -1) {
            wlr_log_errno(WLR_ERROR, "failed to remove state watch for instance %d", instance->id);
        }
        instance->state_wd = -1;
    }
    if (instance->dir_wd > 0) {
        if (inotify_rm_watch(g_inotify, instance->dir_wd) == -1) {
            wlr_log_errno(WLR_ERROR, "failed to remove dir watch for instance %d", instance->id);
        }
        instance->dir_wd = -1;
    }
}

bool
instance_focus(struct instance *instance) {
    if (instance->reset_wait || instance->state.screen != INWORLD) {
        return false;
    }

    input_focus_window(g_compositor->input, instance->window);

    static const struct synthetic_key unpause_keys[] = {
        {KEY_ESC, true},
        {KEY_ESC, false},
        {KEY_F1, true},
        {KEY_F1, false},
    };
    input_send_keys(instance->window, unpause_keys,
                    g_config->use_f1 ? ARRAY_LEN(unpause_keys) : ARRAY_LEN(unpause_keys) - 2);

    return true;
}

bool
instance_process_inotify(struct instance *instance, const struct inotify_event *event) {
    if (event->wd == instance->dir_wd) {
        if (instance->state_fd <= 0 && event->mask & IN_CREATE) {
            if (instance->mods.state_output) {
                // An instance's wpstateout.txt file may not exist when the window is first opened.
                if (strcmp(event->name, "wpstateout.txt") == 0) {
                    open_state_file(instance);
                }
            } else {
                // Watch for the recreation of latest.log at midnight.
                if (strcmp(event->name, "latest.log") == 0) {
                    open_state_file(instance);
                }
            }
        }
        return true;
    } else if (event->wd == instance->state_wd) {
        if (event->mask & IN_MODIFY) {
            // wpstateout.txt or latest.log was modified - process the update.
            if (instance->mods.state_output) {
                process_state_update(instance);
            } else {
                process_log_update(instance);
            }
        } else if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
            // State file deleted or moved - This is normal when state-output is not in use, since
            // Minecraft automatically creates a new `latest.log` file at midnight.
            close(instance->state_fd);
            instance->state_fd = -1;
            if (event->mask & IN_MOVE_SELF) {
                inotify_rm_watch(g_inotify, instance->state_wd);
                instance->state_wd = -1;
            }

            if (instance->mods.state_output) {
                wlr_log(WLR_ERROR,
                        "instance %d uses state output but its wpstateout.txt file was deleted",
                        instance->id);
            }
        }
        return true;
    } else {
        return false;
    }
}

bool
instance_read_options(struct instance *instance) {
    char optbuf[PATH_MAX];
    struct str opt_path = str_new(optbuf, PATH_MAX);
    opt_path = str_copy(opt_path, str_from(instance->dir));
    opt_path = str_appendl(opt_path, "/options.txt");

    FILE *file = fopen(str_into(opt_path), "r");
    if (!file) {
        wlr_log_errno(WLR_ERROR, "failed to open options file ('%s')", str_into(opt_path));
        return false;
    }

    char line[1024];
    struct {
        bool atum_hotkey, fullscreen_hotkey, wp_hotkey;
        bool gui_scale, unicode;
    } found = {0};
    struct instance_options options = instance->options;

    while (fgets(line, STRING_LEN(line), file)) {
        const char atum_hotkey[] = "key_Create New World:";
        const char fullscreen_hotkey[] = "key_key.fullscreen:";
        const char wp_hotkey[] = "key_Leave Preview:";
        const char gui_scale[] = "guiScale:";
        const char unicode[] = "forceUnicodeFont:";

        char *newline = strrchr(line, '\n');
        if (newline) {
            *newline = '\0';
        }

        uint8_t *keycode;
        if (strncmp(line, atum_hotkey, STRING_LEN(atum_hotkey)) == 0) {
            found.atum_hotkey = true;
            const char *key = line + STRING_LEN(atum_hotkey);
            if (!(keycode = get_mc_keycode(key))) {
                wlr_log(WLR_INFO, "unknown atum hotkey '%s' in '%s': setting to default of F6", key,
                        str_into(opt_path));
            }
            options.hotkeys.atum_reset = keycode ? *keycode : KEY_F6;
            continue;
        }
        if (strncmp(line, fullscreen_hotkey, STRING_LEN(fullscreen_hotkey)) == 0) {
            found.fullscreen_hotkey = true;
            const char *key = line + STRING_LEN(fullscreen_hotkey);
            if (!(keycode = get_mc_keycode(key))) {
                wlr_log(WLR_INFO,
                        "unknown fullscreen hotkey '%s' in '%s': setting to default of F11", key,
                        str_into(opt_path));
            }
            options.hotkeys.fullscreen = keycode ? *keycode : KEY_F11;
            continue;
        }
        if (strncmp(line, wp_hotkey, STRING_LEN(wp_hotkey)) == 0) {
            found.wp_hotkey = true;
            const char *key = line + STRING_LEN(wp_hotkey);
            if (!(keycode = get_mc_keycode(key))) {
                wlr_log(WLR_INFO,
                        "unknown leave preview hotkey '%s' in '%s': setting to default of H", key,
                        str_into(opt_path));
            }
            options.hotkeys.leave_preview = keycode ? *keycode : KEY_H;
            continue;
        }

        if (strncmp(line, gui_scale, STRING_LEN(gui_scale)) == 0) {
            found.gui_scale = true;
            const char *scale = line + STRING_LEN(gui_scale);
            if (!(*scale)) {
                wlr_log(WLR_ERROR, "invalid options in '%s': no GUI scale", str_into(opt_path));
                goto fail;
            }

            char *endptr = NULL;
            options.gui_scale = strtol(scale, &endptr, 10);
            if (*endptr) {
                wlr_log_errno(WLR_ERROR, "failed to parse GUI scale ('%s') in '%s'", scale,
                              str_into(opt_path));
                goto fail;
            }
            continue;
        }
        if (strncmp(line, unicode, STRING_LEN(unicode)) == 0) {
            found.unicode = true;
            const char *force_unicode = line + STRING_LEN(unicode);
            if (strcmp(force_unicode, "false") == 0) {
                options.unicode = false;
                continue;
            }
            if (strcmp(force_unicode, "true") == 0) {
                options.unicode = true;
                continue;
            }
            wlr_log(WLR_ERROR, "failed to parse forceUnicodeFont ('%s') in '%s'", force_unicode,
                    str_into(opt_path));
            goto fail;
        }
    }

    if (instance->mods.atum && !found.atum_hotkey) {
        wlr_log(WLR_INFO, "no atum hotkey found in '%s': setting to default of F6",
                str_into(opt_path));
        options.hotkeys.atum_reset = KEY_F6;
    }
    if (!found.fullscreen_hotkey) {
        wlr_log(WLR_INFO, "no fullscreen hotkey found in '%s': setting to default of F11",
                str_into(opt_path));
        options.hotkeys.fullscreen = KEY_F11;
    }
    if (instance->mods.world_preview && !found.wp_hotkey) {
        wlr_log(WLR_INFO, "no leave preview hotkey found in '%s': setting to default of H",
                str_into(opt_path));
        options.hotkeys.leave_preview = KEY_H;
    }
    if (!found.gui_scale) {
        wlr_log(WLR_INFO, "no GUI scale found in '%s': setting to default of auto",
                str_into(opt_path));
        options.gui_scale = 0;
    }
    if (!found.unicode) {
        wlr_log(WLR_INFO, "no forceUnicodeFont found in '%s': setting to default of false",
                str_into(opt_path));
        options.unicode = false;
    }

    instance->options = options;
    return true;

fail:
    fclose(file);
    return false;
}

bool
instance_reset(struct instance *instance) {
    if (g_wall->active_instance != instance->id) {
        // Do not allow resetting on the dirt screen. This can cause an immediate reset as soon as
        // the dirt screen ends.
        if (instance->state.screen == GENERATING || instance->state.screen == WAITING) {
            return false;
        }

        // Do not allow resetting in grace period.
        if (instance->state.screen == PREVIEWING && g_config->grace_period > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);

            int64_t preview_msec =
                instance->last_preview.tv_sec * 1000 + instance->last_preview.tv_nsec / 1000000;
            int64_t now_msec = now.tv_sec * 1000 + now.tv_nsec / 1000000;
            if (now_msec - preview_msec < g_config->grace_period) {
                return false;
            }
        }
    }

    // Atum does not handle the global reset hotkey until the instance's window has been clicked
    // at least once ever.
    if (instance->state.screen == TITLE) {
        input_click(instance->window);
    }

    // Attempt to fix ghost pie.
    if (g_wall->active_instance == instance->id && instance->state.screen == INWORLD) {
        static const struct synthetic_key pie_keys[] = {
            {KEY_ESC, true},         {KEY_ESC, false}, {KEY_LEFTSHIFT, false},
            {KEY_RIGHTSHIFT, false}, {KEY_F3, true},   {KEY_F3, false},
        };

        // Only send the Escape key press if a menu is open.
        if (instance->state.data.inworld == UNPAUSED) {
            input_send_keys(instance->window, pie_keys + 2, ARRAY_LEN(pie_keys) - 2);
        } else {
            input_send_keys(instance->window, pie_keys, ARRAY_LEN(pie_keys));
        }
    }

    uint32_t reset_keycode = instance->state.screen == PREVIEWING
                                 ? instance->options.hotkeys.leave_preview
                                 : instance->options.hotkeys.atum_reset;
    const struct synthetic_key reset_keys[] = {
        {reset_keycode, true},
        {reset_keycode, false},
    };
    input_send_keys(instance->window, reset_keys, ARRAY_LEN(reset_keys));

    instance->reset_wait = true;

    return true;
}

struct instance
instance_try_from(struct window *window, bool *err) {
    static_assert(sizeof(int) >= sizeof(pid_t), "sizeof(int) < sizeof(pid_t)");
    ww_assert(err);
    *err = false;

    struct instance instance = {0};

    pid_t pid = window_get_pid(window);
    char buf[PATH_MAX], dir[PATH_MAX];

    ssize_t n = snprintf(buf, PATH_MAX, "/proc/%d/cwd", (int)pid);
    ww_assert(n < PATH_MAX);
    n = readlink(buf, dir, PATH_MAX - 1);
    ww_assert(n < PATH_MAX - 1);
    if (n == -1) {
        wlr_log_errno(WLR_ERROR, "failed to readlink instance directory (pid %d)", (int)pid);
        *err = true;
        return instance;
    }
    dir[n] = '\0';

    if (!check_instance_dirs(dir)) {
        *err = true;
        return instance;
    }

    instance.window = window;
    instance.dir = strdup(dir);
    if (!get_version(&instance)) {
        goto fail;
    }
    if (!get_mods(&instance)) {
        goto fail;
    }
    if (!instance_read_options(&instance)) {
        goto fail;
    }

    instance.state_fd = instance.state_wd = -1;
    if (instance.mods.state_output) {
        instance.dir_wd = inotify_add_watch(g_inotify, instance.dir, IN_CREATE);
    } else {
        char buf[PATH_MAX];
        struct str path = str_new(buf, PATH_MAX);
        path = str_copy(path, str_from(instance.dir));
        path = str_appendl(path, "/logs/");
        instance.dir_wd = inotify_add_watch(g_inotify, str_into(path), IN_CREATE);
    }
    if (instance.dir_wd == -1) {
        wlr_log_errno(WLR_ERROR, "failed to create directory watch for instance at '%s'",
                      instance.dir);
        goto fail;
    }
    open_state_file(&instance);

    *err = false;
    return instance;

fail:
    free(instance.dir);
    *err = true;
    return instance;
}
