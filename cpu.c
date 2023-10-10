#include "cpu.h"
#include "util.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/util/log.h>

// TODO: simple optimizations (e.g. keep cgroup.procs open)

static const char cgroup_dir[] = "/sys/fs/cgroup/waywall/";
static const char *group_names[] = {
    [CPU_NONE] = "none", [CPU_IDLE] = "idle",     [CPU_LOW] = "low",
    [CPU_HIGH] = "high", [CPU_ACTIVE] = "active",
};

static struct {
    pid_t pid;
    enum cpu_group group;
} pid_groups[MAX_INSTANCE_COUNT];
static bool any_active;

static int
find_pid(pid_t pid) {
    for (int i = 0; i < MAX_INSTANCE_COUNT; i++) {
        if (pid_groups[i].pid == pid || pid_groups[i].pid == 0) {
            return i;
        }
    }
    ww_unreachable();
}

static const char *
strgroup(enum cpu_group group) {
    ww_assert(group >= CPU_NONE && group <= CPU_ACTIVE);

    return group_names[group];
}

static bool
check_dir(const char *dirname) {
    DIR *dir = opendir(dirname);
    if (!dir) {
        wlr_log_errno(WLR_ERROR, "cpu_init: failed to open directory '%s'", dirname);
        return false;
    }
    closedir(dir);
    struct stat dstat = {0};
    if (stat(dirname, &dstat) != 0) {
        wlr_log_errno(WLR_ERROR, "cpu_init: failed to stat directory '%s'", dirname);
        return false;
    }
    if (geteuid() != dstat.st_uid) {
        wlr_log(WLR_ERROR, "cpu_init: directory '%s' not owned by current user", dirname);
        return false;
    }
    return true;
}

static bool
check_file(const char *filename) {
    struct stat fstat = {0};
    if (stat(filename, &fstat) != 0) {
        wlr_log_errno(WLR_ERROR, "cpu_init: failed to stat file '%s'", filename);
        return false;
    }
    if (!S_ISREG(fstat.st_mode)) {
        wlr_log_errno(WLR_ERROR, "cpu_init: file '%s' is directory", filename);
        return false;
    }
    if (geteuid() != fstat.st_uid) {
        wlr_log(WLR_ERROR, "cpu_init: file '%s' not owned by current user", filename);
        return false;
    }
    return true;
}

bool
cpu_init() {
    if (!check_dir(cgroup_dir)) {
        return false;
    }
    for (size_t i = 1; i < ARRAY_LEN(group_names); i++) {
        char buf[PATH_MAX];
        strcpy(buf, cgroup_dir);
        size_t buflen = STRING_LEN(cgroup_dir) + strlen(group_names[i]) + 2;
        ww_assert(buflen < ARRAY_LEN(buf));
        strcat(buf, group_names[i]);
        if (!check_dir(buf)) {
            return false;
        }
        buflen += STRING_LEN("/cpu.weight") + 2;
        ww_assert(buflen < ARRAY_LEN(buf));
        strcat(buf, "/cpu.weight");
        if (!check_file(buf)) {
            return false;
        }
    }
    return true;
}

void
cpu_move_to_group(pid_t pid, enum cpu_group group) {
    static_assert(sizeof(int) >= sizeof(pid_t), "sizeof(int) < sizeof(pid_t)");
    ww_assert(group != CPU_NONE);

    int i = find_pid(pid);
    if (pid_groups[i].group == group) {
        return;
    }

    // There should only be one active instance at any point.
    if (group == CPU_ACTIVE) {
        ww_assert(!any_active);
    }

    if (pid_groups[i].group == CPU_ACTIVE) {
        any_active = false;
    } else if (group == CPU_ACTIVE) {
        any_active = true;
    }
    pid_groups[i].group = group;

    char buf[PATH_MAX];
    ww_assert(STRING_LEN(cgroup_dir) + strlen(strgroup(group)) + STRING_LEN("/cgroup.procs") + 1 <
              ARRAY_LEN(buf));
    strcpy(buf, cgroup_dir);
    strcat(buf, strgroup(group));
    strcat(buf, "/cgroup.procs");

    int fd = open(buf, O_WRONLY);
    if (fd == -1) {
        wlr_log_errno(WLR_ERROR, "failed to open cgroup.procs of group %s for pid %d",
                      strgroup(group), (int)pid);
        return;
    }
    size_t n = snprintf(buf, ARRAY_LEN(buf), "%d", (int)pid);
    ww_assert(n < ARRAY_LEN(buf) - 1);
    if (write(fd, buf, n) == -1) {
        wlr_log_errno(WLR_ERROR, "failed to write cgroup.procs of group %s for pid %d",
                      strgroup(group), (int)pid);
    }
    close(fd);
}

bool
cpu_set_group_weight(enum cpu_group group, int weight) {
    char buf[PATH_MAX];
    size_t buflen =
        STRING_LEN(cgroup_dir) + strlen(strgroup(group)) + STRING_LEN("/cpu.weight") + 1;
    ww_assert(buflen < ARRAY_LEN(buf));
    strcpy(buf, cgroup_dir);
    strcat(buf, strgroup(group));
    strcat(buf, "/cpu.weight");
    int fd = open(buf, O_WRONLY);
    if (fd == -1) {
        wlr_log_errno(WLR_ERROR, "cpu_set_group_parameters: failed to open cpu.weight");
        return false;
    }
    size_t n = snprintf(buf, ARRAY_LEN(buf), "%d", weight);
    ww_assert(n < ARRAY_LEN(buf) - 1);
    if (write(fd, buf, n) == -1) {
        wlr_log_errno(WLR_ERROR, "failed to write cpu.weight of group %s", strgroup(group));
        close(fd);
        return false;
    }
    close(fd);
    return true;
}
