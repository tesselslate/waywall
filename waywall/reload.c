#include "reload.h"
#include "config/config.h"
#include "inotify.h"
#include "util/log.h"
#include "util/prelude.h"
#include <dirent.h>
#include <stdint.h>
#include <sys/inotify.h>

static void
handle_config_file(int wd, uint32_t mask, const char *name, void *data) {
    struct reload *rl = data;

    // The file is gone and this watch descriptor should be removed from the list.
    if (mask & IN_IGNORED) {
        for (ssize_t i = 0; i < rl->config_wd.len; i++) {
            if (rl->config_wd.data[i] != wd) {
                continue;
            }

            memmove(rl->config_wd.data + i, rl->config_wd.data + i + 1,
                    sizeof(int) * (rl->config_wd.len - i - 1));
            rl->config_wd.len--;
            return;
        }

        ww_panic("watch descriptor not present in array");
    }

    struct config *cfg = config_create();
    if (config_load(cfg, rl->profile) != 0) {
        ww_log(LOG_ERROR, "failed to load new config");
        config_destroy(cfg);
        return;
    }

    rl->func(cfg, rl->data);
}

static int
add_config_watch(struct reload *rl, const char *name) {
    str path = str_new();
    str_append(&path, rl->config_path);
    str_append(&path, name);

    int wd = inotify_subscribe(rl->inotify, path, IN_CLOSE_WRITE, handle_config_file, rl);
    if (wd == -1) {
        ww_log(LOG_ERROR, "failed to watch config file '%s'", path);
        str_free(path);
        return 1;
    }
    str_free(path);

    if (rl->config_wd.len == rl->config_wd.cap) {
        ww_assert(rl->config_wd.cap > 0);

        ssize_t cap = rl->config_wd.cap * 2;
        int *new_data = realloc(rl->config_wd.data, sizeof(int) * cap);
        check_alloc(new_data);

        rl->config_wd.data = new_data;
        rl->config_wd.cap *= 2;
    }

    rl->config_wd.data[rl->config_wd.len++] = wd;
    return 0;
}

static void
rm_config_watch(struct reload *rl, int wd) {
    for (ssize_t i = 0; i < rl->config_wd.len; i++) {
        if (rl->config_wd.data[i] != wd) {
            continue;
        }

        inotify_unsubscribe(rl->inotify, wd);
        memmove(rl->config_wd.data + i, rl->config_wd.data + i + 1,
                sizeof(int) * (rl->config_wd.len - i - 1));
        rl->config_wd.len--;
        return;
    }

    ww_panic("received unknown config watch wd");
}

static void
handle_config_dir(int wd, uint32_t mask, const char *name, void *data) {
    struct reload *rl = data;

    if (mask & IN_DELETE_SELF) {
        ww_log(LOG_WARN, "config directory was deleted - automatic reloads will no longer occur");
        inotify_unsubscribe(rl->inotify, wd);
        return;
    }

    // Ignore files that aren't Lua.
    const char *ext = strrchr(name, '.');
    if (!ext || strcmp(ext, ".lua") != 0) {
        return;
    }

    if (mask & IN_CREATE) {
        add_config_watch(rl, name);
    } else if (mask & IN_DELETE) {
        rm_config_watch(rl, wd);
    }
}

struct reload *
reload_create(struct inotify *inotify, const char *profile, reload_func_t callback, void *data) {
    ww_assert(callback);

    struct reload *rl = zalloc(1, sizeof(*rl));
    rl->inotify = inotify;
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

    rl->config_dir_wd =
        inotify_subscribe(rl->inotify, rl->config_path, IN_CREATE | IN_DELETE | IN_DELETE_SELF,
                          handle_config_dir, rl);
    if (rl->config_dir_wd == -1) {
        ww_log(LOG_ERROR, "failed to watch config dir");
        goto fail_watchdir;
    }

    rl->config_wd.data = zalloc(8, sizeof(int));
    rl->config_wd.len = 0;
    rl->config_wd.cap = 8;

    DIR *dir = opendir(rl->config_path);
    if (!dir) {
        ww_log(LOG_ERROR, "failed to open config dir '%s'", rl->config_path);
        goto fail_opendir;
    }

    struct dirent *dirent;
    while ((dirent = readdir(dir))) {
        if (dirent->d_name[0] == '.') {
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
    free(rl->config_wd.data);
    inotify_unsubscribe(rl->inotify, rl->config_dir_wd);

fail_watchdir:
fail_path:
    str_free(rl->config_path);
    free(rl);
    return NULL;
}

void
reload_destroy(struct reload *rl) {
    for (ssize_t i = 0; i < rl->config_wd.len; i++) {
        inotify_unsubscribe(rl->inotify, rl->config_wd.data[i]);
    }
    free(rl->config_wd.data);
    str_free(rl->config_path);
    free(rl);
}
