#include "waywall.h"
#include "compositor.h"
#include "config.h"
#include "ninb.h"
#include "wall.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <unistd.h>

struct compositor *g_compositor;
struct config *g_config;
int g_inotify;
struct wall *g_wall;

static int config_wd;

#define WAYWALL_DISPLAY_PATH "/tmp/waywall-display"

static void
process_config_inotify(const struct inotify_event *event) {
    // TODO: warn if keyboard options have changed since xwayland doesnt care
    if (strcmp(event->name, config_filename) != 0) {
        return;
    }

    struct config *new = config_read();
    if (!new) {
        return;
    }

    struct config *old = g_config;
    g_config = new;
    if (!wall_update_config(g_wall)) {
        LOG(LOG_ERROR, "new config not applied");
        free(new);
        g_config = old;
        return;
    }
    compositor_update_config(g_compositor);
    ninb_update_config();

    config_destroy(old);
    LOG(LOG_INFO, "new config applied");
}

static int
handle_inotify(int fd, uint32_t mask, void *data) {
    char buf[4096] __attribute((aligned(__alignof(struct inotify_event))));
    const struct inotify_event *event;

    for (;;) {
        ssize_t n = read(fd, &buf, sizeof(buf));
        if (n == -1 && errno != EAGAIN) {
            LOG_ERRNO(LOG_ERROR, "failed to read inotify fd");
            return 0;
        }
        if (n <= 0) {
            return 0;
        }

        for (char *ptr = buf; ptr < buf + n; ptr += sizeof(struct inotify_event) + event->len) {
            event = (const struct inotify_event *)ptr;
            if (event->wd == config_wd) {
                process_config_inotify(event);
                continue;
            }
            if (wall_process_inotify(g_wall, event)) {
                continue;
            }
        }
    }
}

static int
handle_signal(int signal_number, void *data) {
    switch (signal_number) {
    case SIGINT:
        LOG(LOG_INFO, "received SIGINT; stopping");
        compositor_stop(g_compositor);
        break;
    case SIGTERM:
        LOG(LOG_INFO, "received SIGTERM; stopping");
        compositor_stop(g_compositor);
        break;
    }
    return 0;
}

static _Noreturn void
print_help(int argc, char **argv) {
    fprintf(stderr, "USAGE: %s [--debug]\n", argc ? argv[0] : "waywall");
    exit(1);
}

int
main(int argc, char **argv) {
    int display_file_fd = open(WAYWALL_DISPLAY_PATH, O_WRONLY | O_CREAT, 0644);
    struct flock lock = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
        .l_pid = getpid(),
    };
    if (fcntl(display_file_fd, F_SETLK, &lock) == -1) {
        LOG_ERRNO(LOG_ERROR, "failed to lock waywall-display");
        close(display_file_fd);
        return false;
    }
    ftruncate(display_file_fd, 0);

    g_inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (g_inotify == -1) {
        LOG_ERRNO(LOG_ERROR, "failed to create inotify instance");
        return 1;
    }

    g_config = config_read();
    if (!g_config) {
        return 1;
    }

    char *config_path = config_get_dir();
    if (!config_path) {
        return 1;
    }
    config_wd = inotify_add_watch(g_inotify, config_path, IN_CLOSE_WRITE);
    if (config_wd == -1) {
        LOG_ERRNO(LOG_ERROR, "failed to watch config directory");
        return 1;
    }
    free(config_path);

    g_compositor = compositor_create();
    if (!g_compositor) {
        return 1;
    }
    // TODO: /tmp/waywall-display
    g_compositor->input->sensitivity = g_config->main_sens;

    struct wl_event_loop *loop = wl_display_get_event_loop(g_compositor->display);
    struct wl_event_source *inotify =
        wl_event_loop_add_fd(loop, g_inotify, WL_EVENT_READABLE, handle_inotify, NULL);
    struct wl_event_source *evt_sigint =
        wl_event_loop_add_signal(loop, SIGINT, handle_signal, NULL);
    struct wl_event_source *evt_sigterm =
        wl_event_loop_add_signal(loop, SIGTERM, handle_signal, NULL);

    ninb_init();
    g_wall = wall_create();
    if (!g_wall) {
        return 1;
    }

    bool success = compositor_run(g_compositor);

    wl_event_source_remove(inotify);
    wl_event_source_remove(evt_sigint);
    wl_event_source_remove(evt_sigterm);
    wall_destroy(g_wall);
    compositor_destroy(g_compositor);
    config_destroy(g_config);
    close(g_inotify);
    close(display_file_fd);
    remove(WAYWALL_DISPLAY_PATH);

    LOG(LOG_INFO, "done");
    return success ? 0 : 1;
}
