#include "cmd.h"
#include "util/log.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

static int
print_help(int argc, char **argv) {
    fprintf(stderr, "USAGE: %s launch COMMAND [ARGS...]\n", argc ? argv[0] : "waywall");
    return 1;
}

int
cmd_exec(int argc, char **argv) {
    if (argc < 3) {
        return print_help(argc, argv);
    }

    int fd = open("/tmp/waywall-display", O_RDONLY);
    if (fd == -1) {
        if (errno == ENOENT) {
            ww_log(LOG_ERROR, "waywall is not running");
        } else {
            ww_log_errno(LOG_ERROR, "failed to open waywall-display");
        }
        return 1;
    }

    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n == -1) {
        ww_log_errno(LOG_ERROR, "failed to read waywall-display");
        return 1;
    }
    buf[n] = '\0';
    close(fd);

    setenv("WAYLAND_DISPLAY", buf, 1);

    execvp(argv[2], &argv[2]);
    ww_log_errno(LOG_ERROR, "execvp failed");

    return 1;
}
