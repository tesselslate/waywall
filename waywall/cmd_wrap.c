#include "cmd.h"
#include "config/config.h"
#include "inotify.h"
#include "reload.h"
#include "server/server.h"
#include "util/log.h"
#include "util/prelude.h"
#include "util/syscall.h"
#include "wrap.h"
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>

struct waywall {
    struct config *cfg;
    struct reload *reload;

    struct server *server;
    struct inotify *inotify;
    struct wrap *wrap;

    struct wl_event_source *src_pidfd;

    pid_t child;
};

static int
handle_pidfd(int32_t fd, uint32_t mask, void *data) {
    struct waywall *ww = data;

    if (waitpid(ww->child, NULL, 0) != ww->child) {
        ww_log_errno(LOG_ERROR, "failed to waitpid on child process");
        wl_event_source_fd_update(ww->src_pidfd, 0);
    } else {
        ww_log(LOG_INFO, "child process ended, shutting down");
        server_shutdown(ww->server);
    }

    return 0;
}

static void
handle_reload(struct config *cfg, void *data) {
    struct waywall *ww = data;
    if (wrap_set_config(ww->wrap, cfg) == 0) {
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

int
cmd_wrap(const char *profile, char **argv) {
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

    ww.wrap = wrap_create(ww.server, ww.cfg);
    if (!ww.wrap) {
        goto fail_wrap;
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
    setenv("WAYLAND_DISPLAY", socket_name, true);

    ww.child = fork();
    if (ww.child == 0) {
        // Child process
        execvp(argv[0], argv);
        ww_log_errno(LOG_ERROR, "failed to exec '%s' in child process", argv[0]);
        exit(EXIT_FAILURE);
    } else if (ww.child == -1) {
        // Parent process (error)
        ww_log_errno(LOG_ERROR, "failed to fork child process");
        goto fail_fork;
    }

    ww_assert(close(STDIN_FILENO) == 0);

    int pidfd = pidfd_open(ww.child, 0);
    if (pidfd == -1) {
        ww_log_errno(LOG_ERROR, "failed to open pidfd for child process");
        goto fail_pidfd;
    }

    ww.src_pidfd = wl_event_loop_add_fd(loop, pidfd, WL_EVENT_READABLE, handle_pidfd, &ww);

    wl_display_run(ww.server->display);

    if (pidfd_send_signal(pidfd, SIGKILL, NULL, 0) != 0) {
        if (errno != ESRCH) {
            ww_log_errno(LOG_ERROR, "failed to kill child process");
        }
    }

    wl_event_source_remove(ww.src_pidfd);
    close(pidfd);
    reload_destroy(ww.reload);
    wrap_destroy(ww.wrap);
    inotify_destroy(ww.inotify);
    wl_event_source_remove(src_sigint);
    server_destroy(ww.server);
    config_destroy(ww.cfg);

    return 0;

fail_pidfd:
fail_fork:
fail_socket:
    reload_destroy(ww.reload);

fail_reload:
    wrap_destroy(ww.wrap);

fail_wrap:
    inotify_destroy(ww.inotify);

fail_inotify:
    wl_event_source_remove(src_sigint);
    server_destroy(ww.server);

fail_server:
fail_config_populate:
    config_destroy(ww.cfg);
    return 1;
}
