#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int
main(int argc, char **argv) {
    if (argc < 2) {
        goto print_help;
    } else if (argc == 2 && strcmp(argv[1], "--try") == 0) {
        goto print_help;
    }

    bool try = strcmp(argv[1], "--try") == 0;
    int argstart = try ? 2 : 1;

    int fd = open("/tmp/waywall-display", O_RDONLY);
    if (fd == -1) {
        if (errno == ENOENT) {
            fprintf(stderr, "waywall is not running\n");
            goto fail;
        }
        perror("failed to open waywall-display");
        goto fail;
    }
    char buf[256];
    ssize_t len = read(fd, buf, 255);
    close(fd);
    if (len == -1) {
        perror("failed to read waywall-display");
        goto fail;
    }
    buf[len] = '\0';
    char *newline = strchr(buf, '\n');
    if (!newline) {
        fprintf(stderr, "invalid waywall-display file (no newline found)\n");
        goto fail;
    }
    *newline = '\0';
    setenv("WAYLAND_DISPLAY", buf, true);
    setenv("DISPLAY", newline + 1, true);

    execvp(argv[argstart], &argv[argstart]);
    perror("exec failed");

    return 0;

fail:
    if (try) {
        execvp(argv[argstart], &argv[argstart]);
        perror("exec failed");
    }
    return 1;

print_help:
    fprintf(stderr, "USAGE: %s [--try] COMMAND [ARGS...]\n", argc ? argv[0] : "waywall-launch");
    return 1;
}
