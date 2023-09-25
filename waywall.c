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
#include <wlr/types/wlr_keyboard.h>
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
    struct state state;

    bool alive;
} instances[128];
static bool locks[128];
static int instance_count;
static int active_instance = -1;
static int last_click = -1;
static bool dead_instance;

static int cursor_x, cursor_y;
static uint32_t held_modifiers;
static bool held_buttons[10]; // FIXME: dont rely on low pointer button count?

static inline int
instance_get_id(struct instance *instance) {
    return instance - instances;
}

static void
instance_pause(struct instance *instance) {
    static const struct compositor_key pause_keys[4] = {
        {KEY_F3, true},
        {KEY_ESC, true},
        {KEY_ESC, false},
        {KEY_F3, false},
    };
    compositor_send_keys(instance->window, pause_keys, 4);
}

static void
instance_play(struct instance *instance) {
    // TODO: F1 option
    static const struct compositor_key unpause_keys[4] = {
        {KEY_ESC, true},
        {KEY_ESC, false},
        {KEY_F1, true},
        {KEY_F1, false},
    };
    active_instance = instance_get_id(instance);
    compositor_send_keys(instance->window, unpause_keys, 4);
    compositor_configure_window(instance->window, 0, 0, INST_WIDTH, INST_HEIGHT);
    compositor_focus_window(compositor, instance->window);
    locks[active_instance] = false;
}

static void
instance_reset(struct instance *instance) {
    // TODO: get reset key
    // TODO: get and use leave preview key
    static const struct compositor_key reset_keys[2] = {
        {KEY_F12, true},
        {KEY_F12, false},
    };

    if (instance->state.screen == TITLE) {
        // Atum requires the window is clicked at least once ever before the reset hotkey will work.
        printf("ON TITLE\n");
        compositor_click(instance->window);
    }
    compositor_send_keys(instance->window, reset_keys, 2);
    int i = instance_get_id(instance);
    if (i == active_instance) {
        compositor_configure_window(instance->window, INST_WALL_WIDTH * (i % WALL_WIDTH),
                                    INST_WALL_HEIGHT * (i / WALL_WIDTH), INST_WALL_WIDTH,
                                    INST_WALL_HEIGHT);
    }
}

static void
wall_focus() {
    compositor_focus_window(compositor, NULL);
    active_instance = -1;
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
                if (last_state.screen == PREVIEWING &&
                    instance_get_id(instance) != active_instance) {
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

static void
process_mouse_input(bool click) {
    if (!held_buttons[BTN_LEFT - BTN_MOUSE]) {
        return;
    }
    int x = cursor_x / INST_WALL_WIDTH, y = cursor_y / INST_WALL_HEIGHT;
    int id = (y * WALL_WIDTH) + x;
    if (!click && id == last_click) {
        return;
    }
    last_click = id;
    if (!instances[id].alive) {
        return;
    }
    switch (held_modifiers) {
    case 0:
        instance_reset(&instances[id]);
        break;
    case WLR_MODIFIER_SHIFT:
        instance_play(&instances[id]);
        break;
    case WLR_MODIFIER_CTRL:
        locks[id] = !locks[id];
        break;
    }
}

static bool
handle_button(struct compositor_button_event event) {
    int id = event.button - BTN_MOUSE;
    ww_assert(id >= 0 && id < 10);
    if (!event.state) {
        held_buttons[id] = false;
        return false;
    }
    if (active_instance >= 0) {
        return false;
    }
    held_buttons[id] = true;
    process_mouse_input(true);

    return true;
}

static bool
handle_key(struct compositor_key_event event) {
    bool in_instance = active_instance >= 0;

    for (int i = 0; i < event.nsyms; i++) {
        xkb_keysym_t sym = event.syms[i];
        switch (sym) {
        case XKB_KEY_D:
            if (event.state && (event.modifiers & (WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT)) ==
                                   (WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT)) {
                if (in_instance) {
                    instance_reset(&instances[active_instance]);
                    wall_focus();
                } else {
                    for (int i = 0; i < instance_count; i++) {
                        if (!locks[i] && instances[i].alive) {
                            instance_reset(&instances[i]);
                        }
                    }
                }
                return true;
            }
            break;
        };
    }
    return false;
}

static void
handle_modifiers(uint32_t modifiers) {
    held_modifiers = modifiers;
}

static void
handle_motion(struct compositor_motion_event event) {
    if (active_instance >= 0) {
        return;
    }
    cursor_x = event.x;
    cursor_y = event.y;
    process_mouse_input(false);
}

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
    int i = instance_count - 1;
    compositor_configure_window(window, INST_WALL_WIDTH * (i % WALL_WIDTH),
                                INST_WALL_HEIGHT * (i / WALL_WIDTH), INST_WALL_WIDTH,
                                INST_WALL_HEIGHT);
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
    wlr_log_init(WLR_INFO, NULL);

    inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd == -1) {
        wlr_log_errno(WLR_ERROR, "failed to create inotify instance");
        return 1;
    }

    struct compositor_vtable vtable = {
        .button = handle_button,
        .key = handle_key,
        .modifiers = handle_modifiers,
        .motion = handle_motion,
        .window = handle_window,
    };
    compositor = compositor_create(vtable);
    ww_assert(compositor);
    event_loop = compositor_get_loop(compositor);

    struct wl_event_source *event_sigint =
        wl_event_loop_add_signal(event_loop, SIGINT, handle_signal, NULL);
    struct wl_event_source *event_sigterm =
        wl_event_loop_add_signal(event_loop, SIGTERM, handle_signal, NULL);
    struct wl_event_source *event_inotify =
        wl_event_loop_add_fd(event_loop, inotify_fd, WL_EVENT_READABLE, handle_inotify, NULL);

    compositor_run(compositor);

    wl_event_source_remove(event_sigint);
    wl_event_source_remove(event_sigterm);
    wl_event_source_remove(event_inotify);
    compositor_destroy(compositor);
    close(inotify_fd);
    return 0;
}
