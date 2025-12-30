#include "instance.h"
#include "server/fake_input.h"
#include "server/server.h"
#include "server/ui.h"
#include "server/wl_seat.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include "util/str.h"
#include "util/zip.h"
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static inline int
parse_percent(char data[static 3]) {
    int x = data[0] - '0';
    if (x < 0 || x > 9) {
        return 0;
    }

    int y = data[1] - '0';
    if (y < 0 || y > 9) {
        return x;
    }

    int z = data[2] - '0';
    if (z < 0 || z > 9) {
        return x * 10 + y;
    }

    return 100;
}

static int
check_subdirs(const char *dirname) {
    static const char *should_exist[] = {
        "logs",
        "resourcepacks",
        "saves",
        "screenshots",
    };

    DIR *dir = opendir(dirname);
    if (!dir) {
        ww_log_errno(LOG_ERROR, "failed to open instance dir '%s'", dirname);
        return 1;
    }

    size_t found = 0;
    struct dirent *dirent;
    while ((dirent = readdir(dir))) {
        for (size_t i = 0; i < STATIC_ARRLEN(should_exist); i++) {
            if (strcmp(dirent->d_name, should_exist[i]) == 0) {
                found += 1;
                break;
            }
        }
    }

    closedir(dir);
    if (found != STATIC_ARRLEN(should_exist)) {
        ww_log(LOG_ERROR, "potential instance dir '%s' is missing directories", dirname);
        return 1;
    }
    return 0;
}

static int
process_mod_zip(const char *path, bool *stateoutput) {
    struct zip *zip = zip_open(path);
    if (!zip) {
        return 1;
    }

    const char *filename;
    while ((filename = zip_next(zip))) {
        if (strcmp(filename, "me/voidxwalker/worldpreview/StateOutputHelper.class") == 0) {
            // WorldPreview with state output (3.x - 4.x)
            *stateoutput = true;
            break;
        } else if (strcmp(filename, "xyz/tildejustin/stateoutput/") == 0) {
            // Legacy state-output
            *stateoutput = true;
            break;
        } else if (strcmp(filename, "dev/tildejustin/stateoutput/") == 0) {
            // state-output
            *stateoutput = true;
            break;
        }
    }

    zip_close(zip);
    return 0;
}

static int
get_mods(const char *dirname, bool *stateoutput) {
    strbuf dirpath = strbuf_new();
    strbuf_append(&dirpath, dirname);
    strbuf_append(&dirpath, "/mods/");

    DIR *dir = opendir(dirpath);
    if (!dir) {
        ww_log_errno(LOG_ERROR, "failed to open instance mods dir '%s'", dirpath);
        goto fail_dir;
    }

    struct dirent *dirent;
    while ((dirent = readdir(dir))) {
        if (dirent->d_name[0] == '.') {
            continue;
        }

        const char *ext = strrchr(dirent->d_name, '.');
        if (!ext || strcmp(ext, ".jar") != 0) {
            continue;
        }

        strbuf modpath = strbuf_new();
        strbuf_append(&modpath, dirpath);
        strbuf_append(&modpath, dirent->d_name);

        if (process_mod_zip(modpath, stateoutput) != 0) {
            strbuf_free(modpath);
            goto fail_zip;
        }

        strbuf_free(modpath);
    }

    closedir(dir);
    strbuf_free(dirpath);
    return 0;

fail_zip:
    closedir(dir);

fail_dir:
    strbuf_free(dirpath);
    return 1;
}

static int
open_state_file(const char *dir) {
    strbuf path = strbuf_new();
    strbuf_append(&path, dir);
    strbuf_append(&path, "/wpstateout.txt");

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
        ww_log_errno(LOG_ERROR, "failed to open state file '%s'", path);
        strbuf_free(path);
        return -1;
    }
    strbuf_free(path);

    return fd;
}

struct instance *
instance_create(struct server_view *view, struct inotify *inotify) {
    static_assert(sizeof(pid_t) <= sizeof(int));

    pid_t pid = server_view_get_pid(view);

    char buf[PATH_MAX];
    ssize_t n = snprintf(buf, STATIC_ARRLEN(buf), "/proc/%d/cwd", (int)pid);
    ww_assert(n < (ssize_t)STATIC_ARRLEN(buf));

    char dir[PATH_MAX];
    n = readlink(buf, dir, STATIC_ARRLEN(dir));
    if (n == -1) {
        ww_log_errno(LOG_ERROR, "failed to readlink instance dir (pid=%d)", (int)pid);
        return nullptr;
    } else if (n >= (ssize_t)STATIC_ARRLEN(dir) - 1) {
        ww_log(LOG_ERROR, "instance dir too long (pid=%d)", (int)pid);
        return nullptr;
    }
    dir[n] = '\0';

    // If this is a real Minecraft instance, it should have some normal directories. This does not
    // guarantee that it is actually an instance, but if you're trying to fool the detection then
    // it's your fault.
    if (check_subdirs(dir) != 0) {
        return nullptr;
    }

    bool stateoutput = false;
    if (get_mods(dir, &stateoutput) != 0) {
        return nullptr;
    }
    if (!stateoutput) {
        ww_log(LOG_WARN, "instance does not have state output");
        return nullptr;
    }

    int state_fd = open_state_file(dir);
    if (state_fd == -1) {
        return nullptr;
    }

    struct instance *instance = zalloc(1, sizeof(*instance));

    instance->dir = strdup(dir);
    check_alloc(instance->dir);

    instance->pid = pid;
    instance->state_fd = state_fd;
    instance->state_wd = -1;
    instance->view = view;

    instance->state.screen = SCREEN_TITLE;

    return instance;
}

void
instance_destroy(struct instance *instance) {
    free(instance->dir);
    free(instance);
}

strbuf
instance_get_state_path(struct instance *instance) {
    strbuf path = strbuf_new();
    strbuf_append(&path, instance->dir);
    strbuf_append(&path, "/wpstateout.txt");

    return path;
}

void
instance_state_update(struct instance *instance) {
    // The longest state which can currently be held by wpstateout.txt is 22 characters long:
    // "inworld,gamescreenopen"
    char buf[24];

    if (lseek(instance->state_fd, 0, SEEK_SET) == -1) {
        ww_log_errno(LOG_ERROR, "failed to seek wpstateout.txt in '%s'", instance->dir);
        return;
    }

    ssize_t n = read(instance->state_fd, buf, STATIC_STRLEN(buf));
    if (n == 0) {
        return;
    } else if (n == -1) {
        ww_log_errno(LOG_ERROR, "failed to read wpstateout.txt in '%s'", instance->dir);
        return;
    }
    buf[n] = '\0';

    struct instance_state next = instance->state;
    switch (buf[0]) {
    case 't': // title
        next.screen = SCREEN_TITLE;
        break;
    case 'w': // waiting | wall
        switch (buf[2]) {
        case 'i':
            next.screen = SCREEN_WAITING;
            break;
        case 'l':
            next.screen = SCREEN_WALL;
            break;
        default:
            ww_log(LOG_ERROR, "cannot parse wpstateout.txt: '%s'", buf);
            return;
        }
        break;
    case 'g': // generating,PERCENT
        next.screen = SCREEN_GENERATING;
        next.data.percent = parse_percent(buf + STATIC_STRLEN("generating,"));
        break;
    case 'p': // previewing,PERCENT
        next.screen = SCREEN_PREVIEWING;
        next.data.percent = parse_percent(buf + STATIC_STRLEN("previewing,"));
        break;
    case 'i': // inworld,DATA
        next.screen = SCREEN_INWORLD;
        switch (buf[STATIC_STRLEN("inworld,")]) {
        case 'u': // unpaused
            next.data.inworld = INWORLD_UNPAUSED;
            break;
        case 'p': // paused
            next.data.inworld = INWORLD_PAUSED;
            break;
        case 'g': // gamescreenopen (menu)
            next.data.inworld = INWORLD_MENU;
            break;
        }
        break;
    default:
        ww_log(LOG_ERROR, "cannot parse wpstateout.txt: '%s'", buf);
        return;
    }

    instance->state = next;
}
