#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

static int
print_help(int argc, char **argv) {
    fprintf(stderr, "USAGE: %s COMMAND [ARGS...]\n", argc ? argv[0] : "waywall-launch");
    return 1;
}

int
main(int argc, char **argv) {
    if (argc < 2) {
        return print_help(argc, argv);
    }

    int fd = open("/tmp/waywall-display", O_RDONLY);
    if (fd == -1) {
        if (errno == ENOENT) {
            fprintf(stderr, "waywall is not running");
        } else {
            perror("failed to open waywall-display");
        }
        return 1;
    }

    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n == -1) {
        perror("failed to read waywall-display");
        return 1;
    }
    buf[n] = '\0';
    close(fd);

    setenv("WAYLAND_DISPLAY", buf, 1);

    execvp(argv[1], &argv[1]);
    perror("execvp failed");

    return 1;
}
