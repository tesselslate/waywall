#include "util/sysinfo.h"
#include "util/log.h"
#include "util/prelude.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>

#define PATH_SYSCTL "/proc/sys/"

#define PATH_INOTIFY_MAX_QUEUED_EVENTS PATH_SYSCTL "fs/inotify/max_queued_events"
#define PATH_INOTIFY_MAX_USER_INSTANCES PATH_SYSCTL "fs/inotify/max_user_instances"
#define PATH_INOTIFY_MAX_USER_WATCHES PATH_SYSCTL "fs/inotify/max_user_watches"

long
number_from_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        ww_log_errno(LOG_ERROR, "failed to open file '%s'", path);
        return -1;
    }

    char buf[128];
    ssize_t n = read(fd, buf, STATIC_ARRLEN(buf));
    close(fd);
    if (n == -1) {
        ww_log_errno(LOG_ERROR, "failed to read file '%s'", path);
        return -1;
    }
    buf[n] = '\0';

    char *endptr;
    long num = strtol(buf, &endptr, 10);
    if (*endptr && *endptr != '\n') {
        ww_log(LOG_ERROR, "invalid terminator on number '%s'", buf);
        return -1;
    }

    return num;
}

static void
log_inotify_limits() {
    long max_queued_events = number_from_file(PATH_INOTIFY_MAX_QUEUED_EVENTS);
    long max_user_instances = number_from_file(PATH_INOTIFY_MAX_USER_INSTANCES);
    long max_user_watches = number_from_file(PATH_INOTIFY_MAX_USER_WATCHES);

    if (max_queued_events == -1 || max_user_instances == -1 || max_user_watches == -1) {
        ww_log(LOG_ERROR, "failed to get inotify limits");
        return;
    }

    ww_log(LOG_INFO, "inotify max queued events:  %ld", max_queued_events);
    ww_log(LOG_INFO, "inotify max user instances: %ld", max_user_instances);
    ww_log(LOG_INFO, "inotify max user watches:   %ld", max_user_watches);
}

static void
log_max_files() {
    struct rlimit limit;
    ww_assert(getrlimit(RLIMIT_NOFILE, &limit) == 0);

    // There isn't much reason to care about the hard limit because we aren't going to raise the
    // soft limit.
    ww_log(LOG_INFO, "max files: %jd", (intmax_t)limit.rlim_cur);
}

static void
log_uname() {
    struct utsname name;
    ww_assert(uname(&name) == 0);

    ww_log(LOG_INFO, "system:  %s", name.sysname);
    ww_log(LOG_INFO, "release: %s", name.release);
    ww_log(LOG_INFO, "version: %s", name.version);
    ww_log(LOG_INFO, "machine: %s", name.machine);
}

void
sysinfo_dump_log() {
    ww_log(LOG_INFO, "---- SYSTEM INFO");

    log_uname();
    log_max_files();
    log_inotify_limits();

    ww_log(LOG_INFO, "---- END SYSTEM INFO");
}
