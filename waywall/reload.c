#include "reload.h"
#include "config/config.h"
#include "inotify.h"
#include "timer.h"
#include "util/alloc.h"
#include "util/list.h"
#include "util/log.h"
#include "util/prelude.h"
#include <dirent.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>

static const struct timespec RELOAD_DEBOUNCE_TIME = {
    .tv_nsec = 100 * 1000000 // 100 milliseconds
};

static int add_config_watch(struct reload *rl, const char *name);
static void rm_config_watch(struct reload *rl, int wd);

static bool
should_watch(const char *name) {
    if (name[0] == '.') {
        return false;
    }

    const char *ext = strrchr(name, '.');
    return ext && strcmp(ext, ".lua") == 0;
}

static void
reload_timer_destroy(void *data) {
    struct reload *rl = data;

    rl->timer_entry = NULL;
}

static void
reload_timer_fire(void *data) {
    struct reload *rl = data;

    ww_timer_entry_destroy(rl->timer_entry);
    rl->timer_entry = NULL;

    struct config *cfg = config_create();
    if (config_load(cfg, rl->profile) != 0) {
        ww_log(LOG_ERROR, "failed to load new config");
        config_destroy(cfg);
        return;
    }

    rl->func(cfg, rl->data);
    ww_log(LOG_INFO, "configuration reloaded");
}

static void
schedule_reload(struct reload *rl) {
    // Debounce reload attempts. This is the simplest way to deal with how certain editors (e.g.
    // gedit) save files by creating a temporary file and replacing the saved file with it, which
    // emits several reload-triggering inotify events in quick succession.
    if (rl->timer_entry) {
        ww_timer_entry_set_duration(rl->timer_entry, RELOAD_DEBOUNCE_TIME);
    } else {
        rl->timer_entry = ww_timer_add_entry(rl->timer, RELOAD_DEBOUNCE_TIME, reload_timer_fire,
                                             reload_timer_destroy, rl);
        if (!rl->timer_entry) {
            ww_log(LOG_ERROR, "failed to create timer entry for reloading configuration");
        }
    }
}

static void
handle_config_dir(int wd, uint32_t mask, const char *name, void *data) {
    struct reload *rl = data;

    if (mask & IN_DELETE_SELF) {
        ww_log(LOG_WARN, "config directory was deleted - automatic reloads will no longer occur");
        reload_disable(rl);
        return;
    }

    if (mask & IN_MOVE_SELF) {
        ww_log(LOG_WARN, "config directory was moved - automatic reloads will no longer occur");
        reload_disable(rl);
        return;
    }

    if (mask & IN_CREATE || mask & IN_MOVED_TO) {
        if (should_watch(name)) {
            add_config_watch(rl, name);
        }
    }
}

static void
handle_config_file(int wd, uint32_t mask, const char *name, void *data) {
    struct reload *rl = data;

    // The file is gone and this watch descriptor should be removed from the list.
    if (mask & IN_IGNORED) {
        rm_config_watch(rl, wd);
        ww_log(LOG_INFO, "removed watch %d", wd);
    }

    schedule_reload(rl);
}

static int
add_config_watch(struct reload *rl, const char *name) {
    str path = str_new();
    str_append(&path, rl->config_path);
    str_append(&path, name);

    int wd = inotify_subscribe(rl->inotify, path, IN_CLOSE_WRITE | IN_MODIFY | IN_MASK_CREATE,
                               handle_config_file, rl);
    if (wd == -1) {
        ww_log(LOG_ERROR, "failed to watch config file '%s'", path);
        str_free(path);
        return 1;
    }
    str_free(path);

    ww_log(LOG_INFO, "added watch for %s (%d)", name, wd);

    list_int_append(&rl->config_wd, wd);
    return 0;
}

static void
rm_config_watch(struct reload *rl, int wd) {
    for (ssize_t i = 0; i < rl->config_wd.len; i++) {
        if (rl->config_wd.data[i] != wd) {
            continue;
        }

        list_int_remove(&rl->config_wd, i);
        return;
    }

    ww_panic("received unknown config watch wd");
}

struct reload *
reload_create(struct inotify *inotify, struct ww_timer *timer, const char *profile,
              reload_func_t callback, void *data) {
    ww_assert(callback);

    struct reload *rl = zalloc(1, sizeof(*rl));
    rl->inotify = inotify;
    rl->timer = timer;
    rl->profile = profile;
    rl->func = callback;
    rl->data = data;

    rl->config_path = str_new();

    const char *env = getenv("XDG_CONFIG_HOME");
    if (env) {
        str_append(&rl->config_path, env);
        str_append(&rl->config_path, "/waywall/");
    } else {
        env = getenv("HOME");
        if (!env) {
            ww_log(LOG_ERROR, "no XDG_CONFIG_HOME or HOME environment variables");
            goto fail_path;
        }

        str_append(&rl->config_path, env);
        str_append(&rl->config_path, "/.config/waywall/");
    }

    rl->config_dir_wd = inotify_subscribe(rl->inotify, rl->config_path,
                                          IN_CREATE | IN_DELETE_SELF | IN_MOVE_SELF | IN_MOVED_TO,
                                          handle_config_dir, rl);
    if (rl->config_dir_wd == -1) {
        ww_log(LOG_ERROR, "failed to watch config dir");
        goto fail_watchdir;
    }

    rl->config_wd = list_int_create();

    DIR *dir = opendir(rl->config_path);
    if (!dir) {
        ww_log(LOG_ERROR, "failed to open config dir '%s'", rl->config_path);
        goto fail_opendir;
    }

    struct dirent *dirent;
    while ((dirent = readdir(dir))) {
        if (!should_watch(dirent->d_name)) {
            continue;
        }

        if (add_config_watch(rl, dirent->d_name) != 0) {
            goto fail_watch;
        }
    }

    closedir(dir);

    return rl;

fail_watch:
    closedir(dir);
    for (ssize_t i = 0; i < rl->config_wd.len; i++) {
        inotify_unsubscribe(rl->inotify, rl->config_wd.data[i]);
    }

fail_opendir:
    list_int_destroy(&rl->config_wd);
    inotify_unsubscribe(rl->inotify, rl->config_dir_wd);

fail_watchdir:
fail_path:
    str_free(rl->config_path);
    free(rl);
    return NULL;
}

void
reload_destroy(struct reload *rl) {
    if (rl->config_dir_wd != -1) {
        reload_disable(rl);
    }
    list_int_destroy(&rl->config_wd);
    str_free(rl->config_path);
    free(rl);
}

void
reload_disable(struct reload *rl) {
    ww_assert(rl->config_dir_wd != -1);

    if (rl->timer_entry) {
        ww_timer_entry_destroy(rl->timer_entry);
    }

    for (ssize_t i = 0; i < rl->config_wd.len; i++) {
        inotify_unsubscribe(rl->inotify, rl->config_wd.data[i]);
    }

    inotify_unsubscribe(rl->inotify, rl->config_dir_wd);
    rl->config_dir_wd = -1;
}
