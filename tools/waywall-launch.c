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

    // This is really not what systemd-run is designed to do, but it does get the instance to spawn
    // in the right cgroup.
    // - We need to pipe through standard I/O (--pipe)
    // - We need to preserve the PWD of this process (--same-dir)
    // - We need to run the instance in the user session (--user)
    // - We need to run the instance in the waywall cgroup (--slice=waywall.slice)
    // - We need to explicitly preserve all of the environment variables (the ungodly amount of
    //   --setenv)
    char **nargv = calloc(8192, sizeof(*nargv));
    if (!nargv) {
        perror("failed to allocate new argv");
        return 1;
    }

    int i = 0;
    nargv[i++] = "systemd-run";
    nargv[i++] = "--pipe";
    nargv[i++] = "--same-dir";
    nargv[i++] = "--user";
    nargv[i++] = "--slice=waywall.slice";
    for (char **env = environ; *env; env++) {
        if (i >= 8192) {
            fprintf(stderr, "too many arguments\n");
            return 1;
        }
        char *buf = calloc(strlen(*env) + strlen("--setenv=") + 1, 1);
        if (!buf) {
            perror("failed to allocate new environment variable arg");
            return 1;
        }
        strcpy(buf, "--setenv=");
        strcat(buf, *env);
        nargv[i++] = buf;
    }
    for (char **arg = argv + 1; *arg; arg++) {
        if (i >= 8192) {
            fprintf(stderr, "too many arguments\n");
            return 1;
        }
        nargv[i++] = *arg;
    }
    execvp("systemd-run", nargv);

    fprintf(stderr, "waywall-launch: failed to call systemd-run. executing normally\n");
    execvp(argv[1], &argv[1]);
    perror("execvp failed");

    return 1;
}
