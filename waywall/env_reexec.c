#include "env_reexec.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include "util/syscall.h"
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#define PASSTHROUGH_FD_ENV "__WAYWALL_ENV_PASSTHROUGH_FD"
#define EXTRA_ENV_SIZE 2

extern char **environ;

static void
add_env(char **env, char *new) {
    char **ptr = env;
    while (*ptr) {
        ptr++;
    }

    *ptr = strdup(new);
    check_alloc(*ptr);

    ww_assert(!(*(++ptr)));
}

static char **
read_env(char *buf, bool skip_displays) {
    static const char *SKIP_ENV[] = {"WAYLAND_DISPLAY", "DISPLAY"};

    size_t count = 0;
    char *ptr = buf;
    while (*ptr) {
        count += 1;
        ptr = strchr(ptr, '\0') + 1;
    }

    char **env = zalloc(count + EXTRA_ENV_SIZE + 1, sizeof(*env));

    ptr = buf;
    for (size_t i = 0; i < count; i++) {
        if (skip_displays) {
            for (size_t j = 0; j < STATIC_ARRLEN(SKIP_ENV); j++) {
                size_t len = strlen(SKIP_ENV[j]);
                if (strncmp(ptr, SKIP_ENV[j], len) == 0 && *(ptr + len) == '=') {
                    i--;
                    count--;
                    goto next;
                }
            }
        }

        env[i] = strdup(ptr);
        check_alloc(env[i]);

    next:
        ptr = strchr(ptr, '\0') + 1;
    }

    return env;
}

void
env_passthrough_add_display(char **env_passthrough) {
    char *wayland_display = getenv("WAYLAND_DISPLAY");
    char *x11_display = getenv("DISPLAY");
    ww_assert(wayland_display && x11_display);

    char buf[256] = {0};
    ssize_t n = snprintf(buf, STATIC_ARRLEN(buf), "WAYLAND_DISPLAY=%s", wayland_display);
    ww_assert(n < (ssize_t)STATIC_ARRLEN(buf));
    add_env(env_passthrough, buf);

    n = snprintf(buf, STATIC_ARRLEN(buf), "DISPLAY=%s", x11_display);
    ww_assert(n < (ssize_t)STATIC_ARRLEN(buf));
    add_env(env_passthrough, buf);

    ww_log(LOG_INFO, "added WAYLAND_DISPLAY=%s to passthrough environment", wayland_display);
    ww_log(LOG_INFO, "added DISPLAY=%s to passthrough environment", x11_display);
}

void
env_passthrough_destroy(char **env_passthrough) {
    for (char **var = env_passthrough; *var; var++) {
        free(*var);
    }
    free(env_passthrough);
}

char **
env_passthrough_get() {
    char *passthrough_fd_env = getenv(PASSTHROUGH_FD_ENV);
    if (!passthrough_fd_env) {
        ww_log(LOG_INFO, "no environment passthrough fd");
        return NULL;
    }
    unsetenv(PASSTHROUGH_FD_ENV);

    int fd = atoi(passthrough_fd_env);
    if (fd <= 0) {
        ww_log(LOG_ERROR, "failed to parse passthrough fd '%s' from env", passthrough_fd_env);
        return NULL;
    }

    ssize_t len = lseek(fd, 0, SEEK_END);
    if (len == -1) {
        ww_log_errno(LOG_ERROR, "failed to get size of passthrough fd");
        goto fail_seek;
    }

    char *buf = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (buf == MAP_FAILED) {
        ww_log_errno(LOG_ERROR, "failed to mmap passthrough fd");
        goto fail_mmap;
    }

    char **env = read_env(buf, true);

    ww_assert(munmap(buf, len) == 0);
    close(fd);

    return env;

fail_mmap:
fail_seek:
    close(fd);
    return NULL;
}

// HACK: PrismLauncher provides several options which modify the environment variables the game is
// started with:
//
//  - "Enable MangoHud"     Modifies LD_PRELOAD to include shared objects from MangoHud
//  - "Use discrete GPU"    Sets environment variables for Nvidia PRIME
//  - "Use Zink"            Sets environment variables to configure Mesa to use Zink
//
// However, since waywall is run as a wrapper command, waywall also gets these environment
// variables. This is probably not what the user intends, and in the case of "Use discrete GPU" is
// often actively harmful, since only the game should use the discrete GPU.
//
// So, this function copies the current environment into a memfd, re-executes waywall with the
// environment of the parent process, and then the restarted waywall process starts the Minecraft
// instance with the environment passed to it via the memfd. This ensures that waywall does not
// inherit any of the environment changes made by PrismLauncher.
//
// If the user doesn't want this behavior, it can be turned off via a flag.
int
env_reexec(char **argv) {
    static const int ENV_SIZE = 1048576;

    // Check whether waywall was already restarted or if doing the environment passthrough restart
    // is unnecessary.
    if (getenv(PASSTHROUGH_FD_ENV)) {
        ww_log(LOG_INFO, "skipping env_reexec (got passthrough fd)");
        return 0;
    }

    for (char **arg = argv; *arg; arg++) {
        if (strcmp(*arg, "--no-env-reexec") == 0) {
            ww_log(LOG_INFO, "skipping env_reexec");
            return 0;
        }
    }

    // Read the parent process' environment from /proc.
    pid_t parent = getppid();
    char path[PATH_MAX];
    ssize_t n = snprintf(path, STATIC_ARRLEN(path), "/proc/%d/environ", (int)parent);
    ww_assert(n < (ssize_t)STATIC_ARRLEN(path));

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
        ww_log_errno(LOG_ERROR, "failed to open parent environment");
        return 1;
    }

    char *penvbuf = zalloc(1, ENV_SIZE);
    n = read(fd, penvbuf, ENV_SIZE);
    if (n == -1) {
        ww_log_errno(LOG_ERROR, "failed to read parent environment");
        goto fail_read;
    } else if (n >= ENV_SIZE - 2) {
        ww_log(LOG_WARN, "parent process environment too large, skipping env_reexec");
        goto fail_read;
    }

    // Create a memfd which will be passed to the restarted waywall process via an environment
    // variable. This memfd will store all of the environment variables which should be set for the
    // game.
    int passthrough_fd = memfd_create("waywall_env_reexec", 0);
    if (passthrough_fd == -1) {
        ww_log(LOG_ERROR, "failed to create environ passthrough fd");
        goto fail_open_passthrough;
    }

    // Copy the current environment into the passthrough fd.
    for (char **var = environ; *var; var++) {
        ssize_t len = strlen(*var) + 1;
        if (write(passthrough_fd, *var, len) != len) {
            ww_log_errno(LOG_ERROR, "failed to write to environment passthrough fd");
            goto fail_write_passthrough;
        }
    }
    if (write(passthrough_fd, "", 1) != 1) {
        ww_log_errno(LOG_ERROR, "failed to terminate environment passthrough fd");
        goto fail_write_passthrough;
    }

    // Create a new environment array storing the parent environment, to be passed to execvpe. Add
    // the passthrough fd to this environment so that we don't enter an infinite loop of reexecuting
    // with a fresh environment.
    char **penv = read_env(penvbuf, false);

    char fd_env[256] = {0};
    snprintf(fd_env, STATIC_ARRLEN(fd_env), "%s=%d", PASSTHROUGH_FD_ENV, passthrough_fd);
    add_env(penv, fd_env);

    ww_log(LOG_INFO, "set passthrough environment fd to %d, restarting", passthrough_fd);
    util_execvpe(argv[0], argv, penv);

    // There's no need to duplicate the error handling cases. If execvpe failed then it's fine to
    // just run through all of the error handling labels to cleanup.
    env_passthrough_destroy(penv);
    ww_log_errno(LOG_ERROR, "env_reexec failed");

fail_write_passthrough:
    close(passthrough_fd);

fail_open_passthrough:
fail_read:
    free(penvbuf);
    close(fd);
    return 1;
}
