#include "config/config.h"
#include "inotify.h"
#include "reload.h"
#include "server/server.h"
#include "string.h"
#include "timer.h"
#include "util/debug.h"
#include "util/log.h"
#include "util/prelude.h"
#include "util/syscall.h"
#include "util/sysinfo.h"
#include "wrap.h"
#include <bits/types/struct_sched_param.h>
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>

struct waywall {
    struct config *cfg;
    struct reload *reload;
    struct ww_timer *timer;

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

        util_debug_enabled = cfg->experimental.debug;
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
cmd_wrap(const char *profile, char **argv) {
    char logname[32] = {0};
    ssize_t n = snprintf(logname, STATIC_ARRLEN(logname), "wrap-%jd", (intmax_t)getpid());
    ww_assert(n < (ssize_t)STATIC_ARRLEN(logname));

    if (!util_debug_init()) {
        return 1;
    }

    int log_fd = util_log_create_file(logname, true);
    if (log_fd == -1) {
        return 1;
    }
    util_log_set_file(log_fd);

    sysinfo_dump_log();

    struct waywall ww = {0};

    ww.cfg = config_create();
    ww_assert(ww.cfg);

    if (config_load(ww.cfg, profile) != 0) {
        goto fail_config_populate;
    }

    util_debug_enabled = ww.cfg->experimental.debug;

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

    ww.timer = ww_timer_create(ww.server);

    ww.wrap = wrap_create(ww.server, ww.inotify, ww.timer, ww.cfg);
    if (!ww.wrap) {
        goto fail_wrap;
    }

    ww.reload = reload_create(ww.inotify, ww.timer, profile, handle_reload, &ww);
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

    int pidfd = pidfd_open(ww.child, 0);
    if (pidfd == -1) {
        ww_log_errno(LOG_ERROR, "failed to open pidfd for child process");
        goto fail_pidfd;
    }
    ww_log(LOG_INFO, "got pidfd %d for child process %jd", pidfd, (intmax_t)ww.child);

    ww.src_pidfd = wl_event_loop_add_fd(loop, pidfd, WL_EVENT_READABLE, handle_pidfd, &ww);

    wl_display_run(ww.server->display);

    if (pidfd_send_signal(pidfd, SIGKILL, NULL, 0) != 0) {
        if (errno != ESRCH) {
            ww_log_errno(LOG_ERROR, "failed to kill child process (pidfd: %d)", pidfd);
        }
    }

    wl_event_source_remove(ww.src_pidfd);
    close(pidfd);
    reload_destroy(ww.reload);
    wrap_destroy(ww.wrap);
    ww_timer_destroy(ww.timer);
    inotify_destroy(ww.inotify);
    wl_event_source_remove(src_sigint);
    server_destroy(ww.server);
    config_destroy(ww.cfg);
    close(log_fd);

    return 0;

fail_pidfd:
fail_fork:
fail_socket:
    reload_destroy(ww.reload);

fail_reload:
    wrap_destroy(ww.wrap);

fail_wrap:
    ww_timer_destroy(ww.timer);
    inotify_destroy(ww.inotify);

fail_inotify:
    wl_event_source_remove(src_sigint);
    server_destroy(ww.server);

fail_server:
fail_config_populate:
    config_destroy(ww.cfg);
    close(log_fd);
    return 1;
}

static void
print_help(const char *argv0) {
    static const char *lines[] = {
        "\nUsage:",
        "\twaywall wrap -- CMD      Run the specified command in a new waywall instance",
        "\nOptions:",
        "\t--profile PROFILE        Run waywall with the given configuration profile",
        "",
    };

    for (size_t i = 0; i < STATIC_ARRLEN(lines); i++) {
        fprintf(stderr, "%s\n", lines[i]);
    }
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
        ww_log_errno(LOG_WARN, "failed to set scheduler priority");
        return;
    }
}

int
main(int argc, char **argv) {
    util_log_init();

    if (argc < 2) {
        print_help(argv[0]);
        return 1;
    }

    const char *action = argv[1];
    const char *profile = NULL;
    char **subcommand = NULL;

    bool expect_profile = false;
    for (int i = 2; i < argc; i++) {
        const char *arg = argv[i];

        if (expect_profile) {
            profile = arg;
            expect_profile = false;
        } else {
            if (arg[0] == '-') {
                if (strcmp(arg, "--profile") == 0) {
                    if (profile) {
                        fprintf(stderr, "can only choose one profile\n");
                        return 1;
                    }
                    expect_profile = true;
                } else if (strcmp(arg, "--") == 0) {
                    subcommand = argv + i + 1;
                    break;
                }
            } else {
                print_help(argv[0]);
                return 1;
            }
        }
    }
    if (expect_profile) {
        fprintf(stderr, "expected PROFILE after --profile\n");
        return 1;
    }

    if (strcmp(action, "wrap") == 0) {
        if (!subcommand || !*subcommand) {
            print_help(argv[0]);
            return 1;
        }

        set_realtime();
        return cmd_wrap(profile, subcommand);
    } else {
        print_help(argv[0]);
        return 1;
    }
}
