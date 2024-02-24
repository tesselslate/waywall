#include "util.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static const char *color_info = "";
static const char *color_warn = "";
static const char *color_err = "";
static const char *color_reset = "";
static void __attribute__((constructor)) init_log() {
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
_ww_log(enum ww_log_level level, const char *fmt, ...) {
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    unsigned long sec = now.tv_sec;
    unsigned long usec = now.tv_nsec / 1000;

    switch (level) {
    case LOG_INFO:
        fprintf(stderr, "%s[%7lu.%06lu] [INFO] ", color_info, sec, usec);
        break;
    case LOG_WARN:
        fprintf(stderr, "%s[%7lu.%06lu] [WARN] ", color_warn, sec, usec);
        break;
    case LOG_ERROR:
        fprintf(stderr, "%s[%7lu.%06lu]  [ERR] ", color_err, sec, usec);
        break;
    }

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "%s\n", color_reset);
}

void
_ww_panic(const char *file, int line, const char *msg) {
    fprintf(stderr, "[%s:%d] %s\n", file, line, msg);
    exit(1);
}
