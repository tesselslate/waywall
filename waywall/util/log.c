#include "util/log.h"
#include "util/prelude.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define PREFIX_INFO "[%7lu.%06lu] [INFO] "
#define PREFIX_WARN "[%7lu.%06lu] [WARN] "
#define PREFIX_ERR "[%7lu.%06lu]  [ERR] "

static const char *color_info = "";
static const char *color_warn = "";
static const char *color_err = "";
static const char *color_reset = "";

static int log_fd = -1;

void
util_log(enum ww_log_level level, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    util_log_va(level, fmt, args, true);
    va_end(args);
}

void
util_log_va(enum ww_log_level level, const char *fmt, va_list args, bool newline) {
    struct timespec now = {0};
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

void
util_log_init() {
    static const char *info = "\x1b[1;34m";
    static const char *warn = "\x1b[1;33m";
    static const char *err = "\x1b[1;31m";
    static const char *reset = "\x1b[0m";

    if (isatty(STDERR_FILENO)) {
        color_info = info;
        color_warn = warn;
        color_err = err;
        color_reset = reset;
    }
}

void
util_log_set_file(int fd) {
    log_fd = fd;
}
