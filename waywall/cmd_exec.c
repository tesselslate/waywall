#include "cmd.h"
#include "util/log.h"
#include "util/prelude.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

extern char **environ;

int
cmd_exec(char **argv) {
    ww_assert(argv[0]);

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

    execvp(argv[0], argv);
    ww_log_errno(LOG_ERROR, "execvp failed");

    return 1;
}
