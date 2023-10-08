#include "instance.h"
#include "compositor.h"
#include "config.h"
#include "util.h"
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include <zip.h>

bool
instance_get_mods(struct instance *instance) {
    // Open the instance's mods directory.
    char buf[PATH_MAX];
    strncpy(buf, instance->dir, PATH_MAX);
    static const char mods[] = "/mods/";
    ssize_t limit = PATH_MAX - strlen(buf) - 1;
    if (limit < (ssize_t)STRING_LEN(mods)) {
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
        if (limit < (ssize_t)strlen(dirent->d_name)) {
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

            // NOTE: This is a bit of a weird method, but I think it's better than a list of
            // hashes or annoying version comparisons (and JSON parsing.)
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
    return true;
}

bool
instance_get_options(struct instance *instance) {
    // Check for the user's Atum and WorldPreview hotkeys.
    char buf[PATH_MAX];
    strncpy(buf, instance->dir, PATH_MAX);
    const char options[] = "/options.txt";
    ssize_t limit = PATH_MAX - strlen(buf) - 1;
    if (limit < (ssize_t)STRING_LEN(options)) {
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
    bool atum_hotkey = false, wp_hotkey = false;
    while (fgets(line, STRING_LEN(line), file)) {
        const char atum[] = "key_Create New World:";
        const char wp[] = "key_Leave Preview:";
        const char gui_scale[] = "guiScale:";
        const char unicode_font[] = "forceUnicodeFont:";

        uint8_t *keycode;
        char *end = strrchr(line, '\n');
        if (end) {
            *end = '\0';
        }
        if (strncmp(line, atum, STRING_LEN(atum)) == 0) {
            const char *key = line + STRING_LEN(atum);
            atum_hotkey = true;
            if ((keycode = get_minecraft_keycode(key))) {
                instance->options.atum_hotkey = *keycode;
            } else {
                wlr_log(WLR_INFO, "unknown atum hotkey '%s' in '%s': setting to default of F6", key,
                        buf);
                instance->options.atum_hotkey = KEY_F6 + 8;
            }
        } else if (strncmp(line, wp, STRING_LEN(wp)) == 0) {
            const char *key = line + STRING_LEN(wp);
            wp_hotkey = true;
            if ((keycode = get_minecraft_keycode(key))) {
                instance->options.preview_hotkey = *keycode;
            } else {
                wlr_log(WLR_INFO,
                        "unknown leave preview hotkey '%s' in '%s': setting to default of H", key,
                        buf);
                instance->options.preview_hotkey = KEY_H + 8;
            }
        } else if (strncmp(line, gui_scale, STRING_LEN(gui_scale)) == 0) {
            const char *scale = line + STRING_LEN(gui_scale);
            char *endptr;
            instance->options.gui_scale = strtol(scale, &endptr, 10);
            if (endptr == scale) {
                wlr_log(WLR_ERROR, "failed to parse GUI scale ('%s')", scale);
                fclose(file);
                return false;
            }
        } else if (strncmp(line, unicode_font, STRING_LEN(unicode_font)) == 0) {
            const char *unicode = line + STRING_LEN(unicode_font);
            if (strcmp(unicode, "false") == 0) {
                instance->options.unicode = false;
            } else if (strcmp(unicode, "true") == 0) {
                instance->options.unicode = true;
            } else {
                wlr_log(WLR_ERROR, "failed to parse forceUnicodeFont ('%s')", unicode);
                fclose(file);
                return false;
            }
        }
    }

    if (!atum_hotkey) {
        wlr_log(WLR_INFO, "no atum hotkey found in '%s': setting to default of F6", buf);
        instance->options.atum_hotkey = KEY_F6 + 8;
    }
    if (!wp_hotkey) {
        wlr_log(WLR_INFO, "no leave preview hotkey found in '%s': setting to default of H", buf);
        instance->options.preview_hotkey = KEY_H + 8;
    }

    return true;
}

bool
instance_try_from(struct window *window, struct instance *instance, int inotify_fd) {
    // Find the instance's directory.
    pid_t pid = compositor_window_get_pid(window);
    char buf[PATH_MAX], dir_path[PATH_MAX];
    if (snprintf(buf, PATH_MAX, "/proc/%d/cwd", (int)pid) >= PATH_MAX) {
        wlr_log(WLR_ERROR, "tried to readlink of path longer than 512 bytes");
        return false;
    }
    ssize_t len = readlink(buf, dir_path, PATH_MAX - 1);
    dir_path[len] = '\0';

    // Check to see if this process has normal directories for a Minecraft instance before spewing
    // errors to the console.
    {
        static const char *dir_names[] = {
            "config",
            "logs",
            "mods",
            "saves",
        };
        DIR *dir = opendir(buf);
        if (!dir) {
            wlr_log_errno(WLR_ERROR, "failed to open process directory");
            return false;
        }
        struct dirent *dirent;
        int found_count = 0;
        while ((dirent = readdir(dir))) {
            for (size_t i = 0; i < ARRAY_LEN(dir_names); i++) {
                if (strcmp(dirent->d_name, dir_names[i]) == 0) {
                    found_count++;
                    break;
                }
            }
        }
        closedir(dir);
        if (found_count != ARRAY_LEN(dir_names)) {
            return false;
        }
    }

    // Check that the instance has the relevant mods and hotkeys.
    instance->alive = true;
    instance->dir = strdup(dir_path);
    if (!instance_get_mods(instance)) {
        goto fail_get_mods;
    }
    if (!instance_get_options(instance)) {
        goto fail_get_options;
    }

    // Open the correct file for reading the instance's state.
    static const char stateout[] = "/wpstateout.txt";
    static const char log[] = "/logs/latest.log";
    const char *state_name = instance->has_stateout ? stateout : log;
    size_t limit = PATH_MAX - len - 1;
    if (limit < strlen(state_name)) {
        wlr_log(WLR_ERROR, "instance path too long");
        goto fail_state_path;
    }
    strcat(dir_path, state_name);
    instance->state_fd = open(dir_path, O_CLOEXEC | O_NONBLOCK | O_RDONLY);
    if (instance->state_fd == -1) {
        wlr_log_errno(WLR_ERROR, "failed to open instance state file (%s)", dir_path);
        goto fail_state_open;
    }
    instance->state_wd = inotify_add_watch(inotify_fd, dir_path, IN_MODIFY);
    if (instance->state_wd == -1) {
        wlr_log_errno(WLR_ERROR, "failed to add instance state file (%s) to inotify", dir_path);
        goto fail_inotify_add;
    }
    instance->window = window;
    instance->state.screen = TITLE;

    // Create the headless views for this instance's verification recording.
    instance->hview_inst = compositor_window_make_headless_view(instance->window);
    instance->hview_wp = compositor_window_make_headless_view(instance->window);

    return true;

fail_inotify_add:
    close(instance->state_fd);

fail_state_open:
fail_state_path:
fail_get_options:
fail_get_mods:
    free((void *)instance->dir);
    return false;
}
