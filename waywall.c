#include "compositor.h"
#include "util.h"
#include <dirent.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/util/log.h>

// TODO: config
#define WALL_WIDTH 3
#define WALL_HEIGHT 5
#define INST_WALL_WIDTH 640
#define INST_WALL_HEIGHT 216
#define INST_WIDTH 1920
#define INST_HEIGHT 1080

struct state {
    enum {
        TITLE,
        WAITING,
        GENERATING,
        PREVIEWING,
        INWORLD,
    } screen;
    union {
        int percent;
        enum {
            UNPAUSED,
            PAUSED,
            INVENTORY,
        } world;
    } data;
};

static struct compositor *compositor;
static struct wl_event_loop *event_loop;
static int inotify_fd;

static struct instance {
    struct window *window;
    const char *dir;
    int wd;
    int fd;
    bool alive;
    struct state state;

} instances[128];
static int instance_count;
static bool dead_instance;

static inline void
instance_pause(struct instance *instance) {
    compositor_send_key(instance->window, KEY_F3, true);
    compositor_send_key(instance->window, KEY_ESC, true);
    compositor_send_key(instance->window, KEY_ESC, false);
    compositor_send_key(instance->window, KEY_F3, false);
    wlr_log(WLR_INFO, "Pause");
}

static void
process_state(struct instance *instance) {
    char buf[128];
    if (lseek(instance->fd, 0, SEEK_SET) == -1) {
        wlr_log_errno(WLR_ERROR, "failed to seek wpstateout");
        return;
    }
    ssize_t len = read(instance->fd, buf, 127);
    if (len == 0) {
        return;
    }
    if (len == -1) {
        wlr_log_errno(WLR_ERROR, "failed to read wpstateout");
        return;
    }
    buf[len] = '\0';

    struct state last_state = instance->state;
    if (strcmp(buf, "title") == 0) {
        instance->state.screen = TITLE;
    } else if (strcmp(buf, "waiting") == 0) {
        instance->state.screen = WAITING;
    } else {
        char *a, *b, *ptr;
        for (ptr = buf; *ptr != ','; ptr++) {
            if (ptr - buf == len) {
                wlr_log(WLR_ERROR, "failed to find comma");
                return;
            }
        }
        *ptr = '\0';
        a = buf;
        b = ptr + 1;
        if (strcmp(a, "generating") == 0) {
            instance->state.screen = GENERATING;
            instance->state.data.percent = atoi(b);
        } else if (strcmp(a, "previewing") == 0) {
            if (last_state.screen != PREVIEWING) {
                instance_pause(instance);
            }
            instance->state.screen = PREVIEWING;
            instance->state.data.percent = atoi(b);
        } else if (strcmp(a, "inworld") == 0) {
            instance->state.screen = INWORLD;
            if (strcmp(b, "unpaused") == 0) {
                if (last_state.screen == PREVIEWING) {
                    instance_pause(instance);
                }
                instance->state.data.world = UNPAUSED;
            } else if (strcmp(b, "paused") == 0) {
                instance->state.data.world = PAUSED;
            } else if (strcmp(b, "gamescreenopen") == 0) {
                instance->state.data.world = INVENTORY;
            } else {
                ww_assert(false);
            }
        } else {
            ww_assert(false);
        }
    }
}

static bool
handle_button(struct compositor_button_event event) {
    return false;
}

static bool
handle_key(struct compositor_key_event event) {
    return false;
}

static void
handle_motion(struct compositor_motion_event event) {}

static bool
handle_window(struct window *window, bool map) {
    if (!map) {
        for (int i = 0; i < instance_count; i++) {
            if (instances[i].window == window) {
                // TODO: handle instance death and reboot
                wlr_log(WLR_ERROR, "instance %d died", i);
                dead_instance = true;
                instances[i].alive = false;
                instances[i].window = NULL;
                return false;
            }
        }
        // TODO: handle ninjabrainbot
        wlr_log(WLR_INFO, "ninjabrain bot died");
        return false;
    }

    pid_t pid = compositor_get_window_pid(window);
    char buf[512];
    char dir_path[512];
    if (snprintf(buf, 512, "/proc/%d/cwd", (int)pid) >= 512) {
        wlr_log(WLR_ERROR, "tried to readlink of path longer than 512 bytes");
        return false;
    }
    ssize_t len = readlink(buf, dir_path, 511);
    dir_path[len] = '\0';
    wlr_log(WLR_DEBUG, "checking for instance at %s", dir_path);

    DIR *dir = opendir(dir_path);
    if (!dir) {
        wlr_log_errno(WLR_ERROR, "failed to open instance directory");
        return false;
    }

    struct dirent *dirent;
    bool is_instance = false;
    while ((dirent = readdir(dir))) {
        if (strcmp(dirent->d_name, "wpstateout.txt") == 0) {
            is_instance = true;
            break;
        }
    }
    closedir(dir);

    if (!is_instance) {
        // TODO: handle ninb
        return true;
    }
    struct instance *instance = &instances[instance_count++];
    instance->alive = true;
    instance->dir = strdup(dir_path);
    strncat(dir_path, "/wpstateout.txt", 511 - len);
    instance->fd = open(dir_path, O_CLOEXEC | O_NONBLOCK | O_RDONLY);
    if (instance->fd == -1) {
        wlr_log_errno(WLR_ERROR, "failed to open wpstateout");
        return false;
    }
    instance->wd = inotify_add_watch(inotify_fd, dir_path, IN_MODIFY);
    if (instance->wd == -1) {
        wlr_log_errno(WLR_ERROR, "failed to add wpstateout to inotify");
        return false;
    }
    instance->window = window;
    instance->state.screen = TITLE;

    wlr_log(WLR_INFO, "created instance %d (%s)", instance_count, instance->dir);
    return false;
}

static int
handle_signal(int signal_number, void *data) {
    wlr_log(WLR_INFO, "received signal %d; stopping", signal_number);
    compositor_stop(compositor);
    return 0;
}

static int
handle_inotify(int fd, uint32_t mask, void *data) {
    char buf[4096] __attribute__((__aligned__(__alignof__(struct inotify_event))));
    const struct inotify_event *event;
    for (;;) {
        ssize_t len = read(fd, &buf, sizeof(buf));
        if (len == -1 && errno != EAGAIN) {
            wlr_log_errno(WLR_ERROR, "read inotify fd");
            return 0;
        }
        if (len <= 0) {
            return 0;
        }
        for (char *ptr = buf; ptr < buf + len; ptr += sizeof(struct inotify_event) + event->len) {
            event = (const struct inotify_event *)ptr;
            if (event->mask & IN_MODIFY) {
                for (int i = 0; i < instance_count; i++) {
                    if (instances[i].wd == event->wd) {
                        process_state(&instances[i]);
                    }
                }
            }
        }
    }
}

int
main() {
    wlr_log_init(WLR_DEBUG, NULL);

    inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd == -1) {
        wlr_log_errno(WLR_ERROR, "failed to create inotify instance");
        return 1;
    }

    struct compositor_vtable vtable = {
        .button = handle_button,
        .key = handle_key,
        .motion = handle_motion,
        .window = handle_window,
    };
    compositor = compositor_create(vtable);
    ww_assert(compositor);
    event_loop = compositor_get_loop(compositor);
    wl_event_loop_add_signal(event_loop, SIGINT, handle_signal, NULL);
    wl_event_loop_add_signal(event_loop, SIGTERM, handle_signal, NULL);
    wl_event_loop_add_fd(event_loop, inotify_fd, WL_EVENT_READABLE, handle_inotify, NULL);

    compositor_run(compositor);

    compositor_destroy(compositor);
    close(inotify_fd);
    return 0;
}
