#include "cmd.h"
#include "config/config.h"
#include "cpu/cgroup_setup.h"
#include "inotify.h"
#include "reload.h"
#include "server/server.h"
#include "server/ui.h"
#include "util/log.h"
#include "util/prelude.h"
#include "util/sysinfo.h"
#include "wall.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>

struct waywall {
    struct config *cfg;
    struct reload *reload;

    struct server *server;
    struct inotify *inotify;
    struct wall *wall;
};

static void
handle_reload(struct config *cfg, void *data) {
    struct waywall *ww = data;
    if (wall_set_config(ww->wall, cfg) == 0) {
        config_destroy(ww->cfg);
        ww->cfg = cfg;
    } else {
        ww_log(LOG_ERROR, "failed to apply new config");
        config_destroy(cfg);
    }
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

    char logname[32] = {0};
    ssize_t n = snprintf(logname, STATIC_ARRLEN(logname), "wall-%jd", (intmax_t)getpid());
    ww_assert(n < (ssize_t)STATIC_ARRLEN(logname));

    int log_fd = util_log_create_file(logname, true);
    if (log_fd == -1) {
        return 1;
    }
    util_log_set_file(log_fd);

    sysinfo_dump_log();

    int display_file_fd = open("/tmp/waywall-display", O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
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

    ww.reload = reload_create(ww.inotify, profile, handle_reload, &ww);
    if (!ww.reload) {
        goto fail_reload;
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

    server_ui_show(ww.server->ui);
    wl_display_run(ww.server->display);

    reload_destroy(ww.reload);
    wall_destroy(ww.wall);
    inotify_destroy(ww.inotify);
    wl_event_source_remove(src_sigint);
    server_destroy(ww.server);
    config_destroy(ww.cfg);

    lock.l_type = F_UNLCK;
    fcntl(display_file_fd, F_SETLK, &lock);
    close(display_file_fd);

    close(log_fd);

    return 0;

fail_socket_write:
fail_socket:
    reload_destroy(ww.reload);

fail_reload:
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
    close(log_fd);

    return 1;
}
