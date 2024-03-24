#include "config/config.h"
#include "cpu/cgroup_setup.h"
#include "inotify.h"
#include "server/server.h"
#include "util.h"
#include "wall.h"
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>

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
handle_signal(int signal, void *data) {
    struct server *server = data;
    server_shutdown(server);
    return 0;
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

    struct config *cfg = config_create();
    if (!cfg) {
        goto fail_config_create;
    }
    if (config_load(cfg) != 0) {
        goto fail_config_populate;
    }

    struct server *server = server_create(cfg);
    if (!server) {
        goto fail_server;
    }

    struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
    struct wl_event_source *src_sigint =
        wl_event_loop_add_signal(loop, SIGINT, handle_signal, server);

    struct inotify *inotify = inotify_create(loop);
    if (!inotify) {
        goto fail_inotify;
    }

    struct wall *wall = wall_create(server, inotify, cfg);
    if (!wall) {
        goto fail_wall;
    }

    const char *socket_name = wl_display_add_socket_auto(server->display);
    if (!socket_name) {
        ww_log(LOG_ERROR, "failed to create wayland display socket");
        goto fail_socket;
    }

    if (write(display_file_fd, socket_name, strlen(socket_name)) != (ssize_t)strlen(socket_name)) {
        ww_log_errno(LOG_ERROR, "failed to write waywall-display");
        goto fail_socket_write;
    }
    wl_display_run(server->display);

    wall_destroy(wall);
    inotify_destroy(inotify);
    wl_event_source_remove(src_sigint);
    server_destroy(server);
    config_destroy(cfg);

    lock.l_type = F_UNLCK;
    fcntl(display_file_fd, F_SETLK, &lock);
    close(display_file_fd);

    ww_log(LOG_INFO, "Done");
    return 0;

fail_socket_write:
fail_socket:
    wall_destroy(wall);

fail_wall:
    inotify_destroy(inotify);

fail_inotify:
    wl_event_source_remove(src_sigint);
    server_destroy(server);

fail_server:
fail_config_populate:
    config_destroy(cfg);

fail_config_create:
    lock.l_type = F_UNLCK;
    fcntl(display_file_fd, F_SETLK, &lock);

fail_lock:
    close(display_file_fd);

    return 1;
}

int
main(int argc, char **argv) {
    util_log_init();

    if (argc > 1) {
        if (strcmp(argv[1], "cpu") == 0) {
            // TODO: Non-systemd support
            ww_log(LOG_ERROR, "Non-systemd cgroups support is not implemented yet.");
            return 1;
        }
    }

    set_realtime();

    char *cgroup_base = cgroup_get_base_systemd();
    if (!cgroup_base) {
        ww_log(LOG_ERROR, "failed to get cgroup base directory");
        return 1;
    }
    switch (cgroup_setup_check(cgroup_base)) {
    case 0:
        break;
    case 1:
        if (cgroup_setup_dir(cgroup_base) != 0) {
            free(cgroup_base);
            return 1;
        }
        break;
    case -1:
        ww_log(LOG_ERROR, "failed to check cgroups");
        free(cgroup_base);
        return 1;
    }
    free(cgroup_base);

    return cmd_waywall();
}
