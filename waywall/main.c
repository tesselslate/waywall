#include "config/config.h"
#include "cpu/cgroup_setup.h"
#include "inotify.h"
#include "server/server.h"
#include "util.h"
#include "wall.h"
#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <wayland-server-core.h>

struct waywall {
    struct config *cfg;

    struct server *server;
    struct inotify *inotify;
    struct wall *wall;

    struct str config_path;
    int config_dir_wd;
    struct {
        int *data;
        ssize_t len, cap;
    } config_wd;
};

static void
handle_config_file(int wd, uint32_t mask, const char *name, void *data) {
    struct waywall *ww = data;

    struct config *cfg = config_create();
    if (config_load(cfg) != 0) {
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
    struct str buf = ww->config_path;
    if (!str_append(&buf, name)) {
        ww_log(LOG_ERROR, "path too long for config file '%s'", name);
        return 1;
    }

    int wd = inotify_subscribe(ww->inotify, buf.data, IN_CLOSE_WRITE, handle_config_file, ww);
    if (wd == -1) {
        ww_log(LOG_ERROR, "failed to watch config file '%s'", name);
        return 1;
    }

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
    struct str *config_dir = &ww->config_path;

    const char *env = getenv("XDG_CONFIG_HOME");
    if (env) {
        if (!str_append(config_dir, env)) {
            ww_log(LOG_ERROR, "config path too long");
            return 1;
        }
        if (!str_append(config_dir, "/waywall/")) {
            ww_log(LOG_ERROR, "config path too long");
            return 1;
        }
    } else {
        env = getenv("HOME");
        if (!env) {
            ww_log(LOG_ERROR, "no XDG_CONFIG_HOME or HOME environment variables");
            return 1;
        }

        if (!str_append(config_dir, env)) {
            ww_log(LOG_ERROR, "config path too long");
            return 1;
        }
        if (!str_append(config_dir, "/.config/waywall/")) {
            ww_log(LOG_ERROR, "config path too long");
            return 1;
        }
    }

    ww->config_dir_wd =
        inotify_subscribe(ww->inotify, config_dir->data, IN_CREATE | IN_DELETE | IN_DELETE_SELF,
                          handle_config_dir, ww);
    if (ww->config_dir_wd == -1) {
        ww_log(LOG_ERROR, "failed to watch config dir");
        return 1;
    }

    ww->config_wd.data = zalloc(8, sizeof(int));
    ww->config_wd.len = 0;
    ww->config_wd.cap = 8;

    DIR *dir = opendir(config_dir->data);
    if (!dir) {
        ww_log(LOG_ERROR, "failed to open config dir '%s'", config_dir->data);
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
}

static int
handle_signal(int signal, void *data) {
    struct server *server = data;
    server_shutdown(server);
    return 0;
}

static void
set_realtime() {
    int priority = sched_get_priority_min(SCHED_RR);
    if (priority == -1) {
        ww_log_errno(LOG_ERROR, "failed to get minimum priority for SCHED_RR");
        return;
    }

    const struct sched_param param = {.sched_priority = priority};
    if (sched_setscheduler(getpid(), SCHED_RR, &param) == -1) {
        ww_log_errno(LOG_ERROR, "failed to set scheduler priority");
        return;
    }
}

static int
cmd_waywall() {
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

    ww.cfg = config_create();
    ww_assert(ww.cfg);

    if (config_load(ww.cfg) != 0) {
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

int
main(int argc, char **argv) {
    util_log_init();

    char *cgroup_base = cgroup_get_base();
    if (!cgroup_base) {
        ww_log(LOG_ERROR, "failed to get cgroup base directory");
        return 1;
    }

    if (argc > 1) {
        if (strcmp(argv[1], "cpu") == 0) {
            cgroup_setup_dir(cgroup_base);
            free(cgroup_base);
            return 1;
        }
    }

    set_realtime();

    switch (cgroup_setup_check(cgroup_base)) {
    case 0:
        break;
    case 1:
        ww_log(LOG_ERROR, "cgroups are not prepared - run 'waywall cpu' with root privileges");
        free(cgroup_base);
        return 1;
    case -1:
        ww_log(LOG_ERROR, "failed to check cgroups");
        free(cgroup_base);
        return 1;
    }
    free(cgroup_base);

    return cmd_waywall();
}
