#include "cmd.h"
#include "config/config.h"
#include "cpu/cgroup_setup.h"
#include "inotify.h"
#include "server/server.h"
#include "util/log.h"
#include "util/str.h"
#include "wall.h"
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/inotify.h>
#include <unistd.h>

struct waywall {
    struct config *cfg;

    struct server *server;
    struct inotify *inotify;
    struct wall *wall;

    str config_path;
    int config_dir_wd;
    struct {
        int *data;
        ssize_t len, cap;
    } config_wd;
    const char *profile;
};

static void
handle_config_file(int wd, uint32_t mask, const char *name, void *data) {
    struct waywall *ww = data;

    // The file is gone and this watch descriptor should be removed from the list.
    if (mask & IN_IGNORED) {
        for (ssize_t i = 0; i < ww->config_wd.len; i++) {
            if (ww->config_wd.data[i] != wd) {
                continue;
            }

            memmove(ww->config_wd.data + i, ww->config_wd.data + i + 1,
                    sizeof(int) * (ww->config_wd.len - i - 1));
            ww->config_wd.len--;
            return;
        }

        ww_panic("watch descriptor not present in array");
    }

    struct config *cfg = config_create();
    if (config_load(cfg, ww->profile) != 0) {
        ww_log(LOG_ERROR, "failed to load new config");
        config_destroy(cfg);
        return;
    }

    if (wall_set_config(ww->wall, cfg) == 0) {
        config_destroy(ww->cfg);
        ww->cfg = cfg;
    } else {
        ww_log(LOG_ERROR, "failed to apply new config");
        config_destroy(cfg);
    }
}

static int
add_config_watch(struct waywall *ww, const char *name) {
    str path = str_new();
    str_append(&path, ww->config_path);
    str_append(&path, name);

    int wd = inotify_subscribe(ww->inotify, path, IN_CLOSE_WRITE, handle_config_file, ww);
    if (wd == -1) {
        ww_log(LOG_ERROR, "failed to watch config file '%s'", path);
        str_free(path);
        return 1;
    }
    str_free(path);

    if (ww->config_wd.len == ww->config_wd.cap) {
        ww_assert(ww->config_wd.cap > 0);

        ssize_t cap = ww->config_wd.cap * 2;
        int *new_data = realloc(ww->config_wd.data, sizeof(int) * cap);
        check_alloc(new_data);

        ww->config_wd.data = new_data;
        ww->config_wd.cap *= 2;
    }

    ww->config_wd.data[ww->config_wd.len++] = wd;
    return 0;
}

static void
rm_config_watch(struct waywall *ww, int wd) {
    for (ssize_t i = 0; i < ww->config_wd.len; i++) {
        if (ww->config_wd.data[i] != wd) {
            continue;
        }

        inotify_unsubscribe(ww->inotify, wd);
        memmove(ww->config_wd.data + i, ww->config_wd.data + i + 1,
                sizeof(int) * (ww->config_wd.len - i - 1));
        ww->config_wd.len--;
        return;
    }

    ww_panic("received unknown config watch wd");
}

static void
handle_config_dir(int wd, uint32_t mask, const char *name, void *data) {
    struct waywall *ww = data;

    if (mask & IN_DELETE_SELF) {
        ww_log(LOG_WARN, "config directory was deleted - automatic reloads will no longer occur");
        inotify_unsubscribe(ww->inotify, wd);
        return;
    }

    // Ignore files that aren't Lua.
    const char *ext = strrchr(name, '.');
    if (!ext || strcmp(ext, ".lua") != 0) {
        return;
    }

    if (mask & IN_CREATE) {
        add_config_watch(ww, name);
    } else if (mask & IN_DELETE) {
        rm_config_watch(ww, wd);
    }
}

static int
config_subscribe(struct waywall *ww) {
    ww->config_path = str_new();

    const char *env = getenv("XDG_CONFIG_HOME");
    if (env) {
        str_append(&ww->config_path, env);
        str_append(&ww->config_path, "/waywall/");
    } else {
        env = getenv("HOME");
        if (!env) {
            ww_log(LOG_ERROR, "no XDG_CONFIG_HOME or HOME environment variables");
            return 1;
        }

        str_append(&ww->config_path, env);
        str_append(&ww->config_path, "/.config/waywall/");
    }

    ww->config_dir_wd =
        inotify_subscribe(ww->inotify, ww->config_path, IN_CREATE | IN_DELETE | IN_DELETE_SELF,
                          handle_config_dir, ww);
    if (ww->config_dir_wd == -1) {
        ww_log(LOG_ERROR, "failed to watch config dir");
        return 1;
    }

    ww->config_wd.data = zalloc(8, sizeof(int));
    ww->config_wd.len = 0;
    ww->config_wd.cap = 8;

    DIR *dir = opendir(ww->config_path);
    if (!dir) {
        ww_log(LOG_ERROR, "failed to open config dir '%s'", ww->config_path);
        goto fail_opendir;
    }

    struct dirent *dirent;
    while ((dirent = readdir(dir))) {
        if (dirent->d_name[0] == '.') {
            continue;
        }

        if (add_config_watch(ww, dirent->d_name) != 0) {
            goto fail_watch;
        }
    }

    closedir(dir);

    return 0;

fail_watch:
    closedir(dir);
    for (ssize_t i = 0; i < ww->config_wd.len; i++) {
        inotify_unsubscribe(ww->inotify, ww->config_wd.data[i]);
    }

fail_opendir:
    free(ww->config_wd.data);
    inotify_unsubscribe(ww->inotify, ww->config_dir_wd);

    return 1;
}

static void
config_unsubscribe(struct waywall *ww) {
    for (ssize_t i = 0; i < ww->config_wd.len; i++) {
        inotify_unsubscribe(ww->inotify, ww->config_wd.data[i]);
    }
    free(ww->config_wd.data);
    str_free(ww->config_path);
}

static int
handle_signal(int signal, void *data) {
    struct server *server = data;
    server_shutdown(server);
    return 0;
}

static int
check_cgroups() {
    char *cgroup_base = cgroup_get_base();
    if (!cgroup_base) {
        ww_log(LOG_ERROR, "failed to get cgroup base directory");
        return 1;
    }

    int ret = cgroup_setup_check(cgroup_base);
    free(cgroup_base);
    switch (ret) {
    case 0:
        return 0;
    case 1:
        ww_log(LOG_ERROR, "cgroups are not prepared - run 'waywall cpu' with root privileges");
        return 1;
    case -1:
        ww_log(LOG_ERROR, "failed to check cgroups");
        return 1;
    default:
        ww_unreachable();
    }
}

int
cmd_run(const char *profile) {
    if (check_cgroups() != 0) {
        return 1;
    }

    int display_file_fd = open("/tmp/waywall-display", O_WRONLY | O_CREAT, 0644);
    if (display_file_fd == -1) {
        ww_log_errno(LOG_ERROR, "failed to open /tmp/waywall-display");
        return 1;
    }

    struct flock lock = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
        .l_pid = getpid(),
    };
    if (fcntl(display_file_fd, F_SETLK, &lock) == -1) {
        ww_log_errno(LOG_ERROR, "failed to lock waywall-display");
        goto fail_lock;
        return 1;
    }

    struct waywall ww = {0};

    ww.profile = profile;
    ww.cfg = config_create();
    ww_assert(ww.cfg);

    if (config_load(ww.cfg, profile) != 0) {
        goto fail_config_populate;
    }

    ww.server = server_create(ww.cfg);
    if (!ww.server) {
        goto fail_server;
    }

    struct wl_event_loop *loop = wl_display_get_event_loop(ww.server->display);
    struct wl_event_source *src_sigint =
        wl_event_loop_add_signal(loop, SIGINT, handle_signal, ww.server);

    ww.inotify = inotify_create(loop);
    if (!ww.inotify) {
        goto fail_inotify;
    }

    ww.wall = wall_create(ww.server, ww.inotify, ww.cfg);
    if (!ww.wall) {
        goto fail_wall;
    }

    if (config_subscribe(&ww) != 0) {
        goto fail_watch_config;
    }

    const char *socket_name = wl_display_add_socket_auto(ww.server->display);
    if (!socket_name) {
        ww_log(LOG_ERROR, "failed to create wayland display socket");
        goto fail_socket;
    }

    if (write(display_file_fd, socket_name, strlen(socket_name)) != (ssize_t)strlen(socket_name)) {
        ww_log_errno(LOG_ERROR, "failed to write waywall-display");
        goto fail_socket_write;
    }
    wl_display_run(ww.server->display);

    config_unsubscribe(&ww);
    wall_destroy(ww.wall);
    inotify_destroy(ww.inotify);
    wl_event_source_remove(src_sigint);
    server_destroy(ww.server);
    config_destroy(ww.cfg);

    lock.l_type = F_UNLCK;
    fcntl(display_file_fd, F_SETLK, &lock);
    close(display_file_fd);

    ww_log(LOG_INFO, "Done");
    return 0;

fail_socket_write:
fail_socket:
    config_unsubscribe(&ww);

fail_watch_config:
    wall_destroy(ww.wall);
    str_free(ww.config_path);

fail_wall:
    inotify_destroy(ww.inotify);

fail_inotify:
    wl_event_source_remove(src_sigint);
    server_destroy(ww.server);

fail_server:
fail_config_populate:
    config_destroy(ww.cfg);
    lock.l_type = F_UNLCK;
    fcntl(display_file_fd, F_SETLK, &lock);

fail_lock:
    close(display_file_fd);

    return 1;
}
