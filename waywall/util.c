#include "util.h"

void
_ww_log(enum log_level level, const char *fmt, ...) {
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    fprintf(stderr, "%09d.%04d ", (int)now.tv_sec, (int)now.tv_nsec);

    if (isatty(STDERR_FILENO) && level == LOG_ERROR) {
        fprintf(stderr, "\x1b[1;31m");
    }
    fprintf(stderr, level == LOG_INFO ? "[INFO]  " : "[ERROR] ");

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    if (isatty(STDERR_FILENO)) {
        fprintf(stderr, "\x1b[0m\n");
    } else {
        fprintf(stderr, "\n");
    }
}

bool
ww_util_parse_color(float value[4], const char *in) {
    size_t len = strlen(in);
    bool maybe_valid_rgb = len == 6 || (len == 7 && in[0] == '#');
    bool maybe_valid_rgba = len == 8 || (len == 9 && in[0] == '#');
    if (!maybe_valid_rgb && !maybe_valid_rgba) {
        return false;
    }
    int r, g, b, a;
    if (maybe_valid_rgb) {
        int n = sscanf(in[0] == '#' ? in + 1 : in, "%02x%02x%02x", &r, &g, &b);
        if (n != 3) {
            return false;
        }
        value[0] = r / 255.0;
        value[1] = g / 255.0;
        value[2] = b / 255.0;
        value[3] = 1.0;
    } else {
        int n = sscanf(in[0] == '#' ? in + 1 : in, "%02x%02x%02x%02x", &r, &g, &b, &a);
        if (n != 4) {
            return false;
        }
        value[0] = r / 255.0;
        value[1] = g / 255.0;
        value[2] = b / 255.0;
        value[3] = a / 255.0;
    }
    return true;
}
