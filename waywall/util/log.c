#include "util/log.h"
#include "util/prelude.h"
#include "util/str.h"
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PREFIX_INFO "[%7lu.%06lu] [INFO] "
#define PREFIX_WARN "[%7lu.%06lu] [WARN] "
#define PREFIX_ERR "[%7lu.%06lu]  [ERR] "

static const char *LOG_NAMES[LOG_NAME_COUNT] = {
    [LOG_NAME_WRAP] = "wrap",
    [LOG_NAME_XWAYLAND] = "xwayland",
};

// Limited to 10 so that the trailing number is a single character for simplicity
static constexpr int MAX_LOG_FILE_NUM = 10;

static const char *color_info = "";
static const char *color_warn = "";
static const char *color_err = "";
static const char *color_reset = "";

static int log_fd = -1;

// Return the directory for log files. In order of preference:
//
//   1. $XDG_STATE_HOME/waywall
//   2. $HOME/.local/state/waywall
//   3. /tmp/waywall
static strbuf
get_log_directory() {
    strbuf path = strbuf_new();

    char *xdg_state_home = getenv("XDG_STATE_HOME");
    if (xdg_state_home && *xdg_state_home == '/') {
        strbuf_append(&path, xdg_state_home);
    } else {
        char *home = getenv("HOME");
        if (!home) {
            strbuf_append(&path, "/tmp/waywall");
            return path;
        }

        strbuf_append(&path, home);
        strbuf_append(&path, "/.local/state");
    }

    strbuf_append(&path, "/waywall/");
    return path;
}

static int
make_log_directory(str directory) {
    struct strs dirs = str_split(directory, '/');
    strbuf path = str_clone(str_lit("/"));

    for (ssize_t i = 0; i < dirs.len; i++) {
        if (dirs.data[i].len == 0) {
            continue;
        }

        strbuf_append(&path, dirs.data[i]);
        strbuf_append(&path, '/');

        if (mkdir(path.data, 0755) != 0 && errno != EEXIST) {
            ww_log_errno(LOG_ERROR, "failed to create log directory (step '%s')", path.data);
            goto fail;
        }
    }

    strbuf_free(&path);
    strs_free(dirs);
    return 0;

fail:
    strbuf_free(&path);
    strs_free(dirs);
    return 1;
}

static void
rotate_logs(enum ww_log_name name) {
    strbuf path = get_log_directory();
    strbuf old = strbuf_clone(path);
    strbuf new = strbuf_clone(path);

    // This logic requires that the number is only a single character.
    static_assert(MAX_LOG_FILE_NUM <= 10);
    for (int num = MAX_LOG_FILE_NUM - 2; num >= 0; num--) {
        old.len = path.len;
        strbuf_append(&old, '/');
        strbuf_append(&old, LOG_NAMES[name]);
        strbuf_append(&old, '-');
        strbuf_append(&old, '0' + num);

        new.len = path.len;
        strbuf_append(&new, '/');
        strbuf_append(&new, LOG_NAMES[name]);
        strbuf_append(&new, '-');
        strbuf_append(&new, '0' + num + 1);

        if (rename(old.data, new.data) != 0 && errno != ENOENT) {
            ww_log_errno(LOG_ERROR, "failed to rotate old log file '%s' -> '%s'", old.data,
                         new.data);
            goto cleanup;
        }
    }

cleanup:
    strbuf_free(&path);
    strbuf_free(&old);
    strbuf_free(&new);
}

void
util_log(enum ww_log_level level, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    util_log_va(level, fmt, args, true);
    va_end(args);
}

void
util_log_va(enum ww_log_level level, const char *fmt, va_list args, bool newline) {
    struct timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    unsigned long sec = now.tv_sec;
    unsigned long usec = now.tv_nsec / 1000;

    // Print the message prefix (timestamp, log level).
    switch (level) {
    case LOG_INFO:
        if (log_fd >= 0) {
            dprintf(log_fd, PREFIX_INFO, sec, usec);
        }
        fprintf(stderr, "%s" PREFIX_INFO, color_info, sec, usec);

        break;
    case LOG_WARN:
        if (log_fd >= 0) {
            dprintf(log_fd, PREFIX_WARN, sec, usec);
        }
        fprintf(stderr, "%s" PREFIX_WARN, color_warn, sec, usec);

        break;
    case LOG_ERROR:
        if (log_fd >= 0) {
            dprintf(log_fd, PREFIX_ERR, sec, usec);
        }
        fprintf(stderr, "%s" PREFIX_ERR, color_err, sec, usec);

        break;
    }

    // Print the actual message.
    if (log_fd >= 0) {
        va_list fd_args;
        va_copy(fd_args, args);
        vdprintf(log_fd, fmt, fd_args);
        va_end(fd_args);
    }
    vfprintf(stderr, fmt, args);

    // Print the trailing newline if needed. XKB inserts a newline at the end of its log messages,
    // so we don't want them.
    if (newline && log_fd >= 0) {
        dprintf(log_fd, "\n");
    }
    fprintf(stderr, "%s%s", color_reset, newline ? "\n" : "");
}

int
util_log_create_file(enum ww_log_name name, bool cloexec) {
    strbuf log_path = get_log_directory();
    if (make_log_directory(strbuf_view(log_path)) != 0) {
        return -1;
    }

    rotate_logs(name);

    strbuf_append(&log_path, LOG_NAMES[name]);
    strbuf_append(&log_path, "-0");

    int flags = O_CREAT | O_WRONLY;
    if (cloexec) {
        flags |= O_CLOEXEC;
    }

    int fd = open(log_path.data, flags, 0644);
    strbuf_free(&log_path);

    return fd;
}

void
util_log_init() {
    static constexpr char COLOR_INFO[] = "\x1b[1;34m";
    static constexpr char COLOR_WARN[] = "\x1b[1;33m";
    static constexpr char COLOR_ERR[] = "\x1b[1;31m";
    static constexpr char COLOR_RESET[] = "\x1b[0m";

    if (isatty(STDERR_FILENO)) {
        color_info = COLOR_INFO;
        color_warn = COLOR_WARN;
        color_err = COLOR_ERR;
        color_reset = COLOR_RESET;
    }
}

void
util_log_set_file(int fd) {
    log_fd = fd;
}
