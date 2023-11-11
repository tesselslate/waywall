#include "waywall.h"
#include "ninb.h"
#include "compositor.h"
#include "config.h"
#include "wall.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <wlr/util/log.h>

struct compositor *g_compositor;
struct config *g_config;
int g_inotify;
struct wall *g_wall;

static int config_wd;

#define WAYWALL_DISPLAY_PATH "/tmp/waywall-display"

static struct compositor_config
create_compositor_config() {
    struct compositor_config compositor_config = {
        .repeat_rate = g_config->repeat_rate,
        .repeat_delay = g_config->repeat_delay,
        .floating_opacity = g_config->ninb_opacity,
        .confine_pointer = g_config->confine_pointer,
        .cursor_theme = g_config->cursor_theme,
        .cursor_size = g_config->cursor_size,
        .stop_on_close = !g_config->remain_in_background,
    };
    memcpy(compositor_config.background_color, g_config->background_color, sizeof(float) * 4);
    return compositor_config;
}

static void
process_config_inotify(const struct inotify_event *event) {
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
        wlr_log(WLR_ERROR, "new config not applied");
        free(new);
        g_config = old;
        return;
    }
    compositor_load_config(g_compositor, create_compositor_config());
    ninb_update_config();
    config_destroy(old);
    wlr_log(WLR_INFO, "new config applied");
}

static int
handle_inotify(int fd, uint32_t mask, void *data) {
    char buf[4096] __attribute((aligned(__alignof(struct inotify_event))));
    const struct inotify_event *event;

    for (;;) {
        ssize_t n = read(fd, &buf, sizeof(buf));
        if (n == -1 && errno != EAGAIN) {
            wlr_log_errno(WLR_ERROR, "failed to read inotify fd");
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
    case SIGUSR1:
        wlr_log(WLR_INFO, "received SIGUSR1");
        break;
    case SIGUSR2:
        wlr_log(WLR_INFO, "received SIGUSR2");
        break;
    case SIGINT:
        wlr_log(WLR_INFO, "received SIGINT; stopping");
        compositor_stop(g_compositor);
        break;
    case SIGTERM:
        wlr_log(WLR_INFO, "received SIGTERM; stopping");
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
    enum wlr_log_importance log_level = WLR_INFO;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            if (log_level == WLR_DEBUG) {
                print_help(argc, argv);
            }
            log_level = WLR_DEBUG;
        } else {
            print_help(argc, argv);
        }
    }
    wlr_log_init(log_level, NULL);

    int display_file_fd = open(WAYWALL_DISPLAY_PATH, O_WRONLY | O_CREAT, 0644);
    struct flock lock = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
        .l_pid = getpid(),
    };
    if (fcntl(display_file_fd, F_SETLK, &lock) == -1) {
        wlr_log_errno(WLR_ERROR, "failed to lock waywall-display");
        close(display_file_fd);
        return false;
    }
    ftruncate(display_file_fd, 0);

    g_inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (g_inotify == -1) {
        wlr_log_errno(WLR_ERROR, "failed to create inotify instance");
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
        wlr_log_errno(WLR_ERROR, "failed to watch config directory");
        return 1;
    }
    free(config_path);

    g_compositor = compositor_create(create_compositor_config());
    if (!g_compositor) {
        return 1;
    }
    input_set_sensitivity(g_compositor->input, g_config->main_sens);

    struct wl_event_loop *loop = compositor_get_loop(g_compositor);
    struct wl_event_source *inotify =
        wl_event_loop_add_fd(loop, g_inotify, WL_EVENT_READABLE, handle_inotify, NULL);
    struct wl_event_source *evt_sigint =
        wl_event_loop_add_signal(loop, SIGINT, handle_signal, NULL);
    struct wl_event_source *evt_sigterm =
        wl_event_loop_add_signal(loop, SIGTERM, handle_signal, NULL);
    struct wl_event_source *evt_sigusr1 =
        wl_event_loop_add_signal(loop, SIGUSR1, handle_signal, NULL);
    struct wl_event_source *evt_sigusr2 =
        wl_event_loop_add_signal(loop, SIGUSR2, handle_signal, NULL);

    ninb_init();
    g_wall = wall_create();
    if (!g_wall) {
        return 1;
    }

    bool success = compositor_run(g_compositor, display_file_fd);

    wl_event_source_remove(inotify);
    wl_event_source_remove(evt_sigint);
    wl_event_source_remove(evt_sigterm);
    wl_event_source_remove(evt_sigusr1);
    wl_event_source_remove(evt_sigusr2);
    wall_destroy(g_wall);
    compositor_destroy(g_compositor);
    config_destroy(g_config);
    close(g_inotify);
    close(display_file_fd);
    remove(WAYWALL_DISPLAY_PATH);
    wlr_log(WLR_INFO, "done");
    return success ? 0 : 1;
}
