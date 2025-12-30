#include "env_reexec.h"
#include "util/alloc.h"
#include "util/list.h"
#include "util/log.h"
#include "util/prelude.h"
#include "util/str.h"
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

static constexpr char PASSTHROUGH_FD_ENV[] = "__WAYWALL_ENV_PASSTHROUGH_FD";

extern char **environ;

struct envvar {
    char *name;
    char *value;
};

LIST_DEFINE(struct envvar, list_envvar);
LIST_DEFINE_IMPL(struct envvar, list_envvar);

static struct list_envvar passthrough_env;
static char **passthrough_envlist;
static bool used_passthrough = false;

static bool
colon_var_contains(char *orig_env, char *needle) {
    char *env = strdup(orig_env);
    check_alloc(env);

    char *env_orig = env;

    for (;;) {
        char *next = strchr(env, ':');
        if (next) {
            *next = '\0';
        }

        if (strcmp(needle, env) == 0) {
            free(env_orig);
            return true;
        }

        if (!next) {
            break;
        }
        env = next + 1;
    }

    free(env_orig);
    return false;
}

static void
envlist_destroy(char **envlist) {
    for (char **var = envlist; *var; var++) {
        free(*var);
    }
    free(envlist);
}

static char **
penv_to_envlist(struct list_envvar *penv) {
    char **envlist = zalloc(penv->len + 1, sizeof(*envlist));
    for (ssize_t i = 0; i < penv->len; i++) {
        str s = str_new();
        str_append(&s, penv->data[i].name);
        str_append(&s, "=");
        str_append(&s, penv->data[i].value);

        envlist[i] = strdup(s);
        check_alloc(envlist[i]);
        str_free(s);
    }

    return envlist;
}

static void
penv_destroy(struct list_envvar *penv) {
    for (ssize_t i = 0; i < penv->len; i++) {
        free(penv->data[i].name);
        free(penv->data[i].value);
    }

    list_envvar_destroy(penv);
}

static ssize_t
penv_find(struct list_envvar *penv, const char *name) {
    for (ssize_t i = 0; i < penv->len; i++) {
        if (strcmp(penv->data[i].name, name) == 0) {
            return i;
        }
    }

    return -1;
}

static char *
penv_get(struct list_envvar *penv, const char *name) {
    ssize_t idx = penv_find(penv, name);
    return (idx >= 0) ? penv->data[idx].value : nullptr;
}

static void
penv_set(struct list_envvar *penv, const char *name, const char *value) {
    char *dup_value = strdup(value);
    check_alloc(dup_value);

    ssize_t idx = penv_find(penv, name);
    if (idx >= 0) {
        free(penv->data[idx].value);
        penv->data[idx].value = dup_value;
        return;
    }

    char *dup_name = strdup(name);
    check_alloc(dup_name);
    list_envvar_append(penv, (struct envvar){dup_name, dup_value});
}

static bool
penv_unset(struct list_envvar *penv, const char *name) {
    for (ssize_t i = 0; i < penv->len; i++) {
        if (strcmp(penv->data[i].name, name) == 0) {
            free(penv->data[i].name);
            free(penv->data[i].value);
            list_envvar_remove(penv, i);
            return true;
        }
    }

    return false;
}

static struct list_envvar
penv_read(char *buf) {
    struct list_envvar penv = list_envvar_create();

    char *ptr = buf;
    while (*ptr) {
        char *split = strchr(ptr, '=');
        if (!split) {
            ww_log(LOG_ERROR, "failed to parse environment text");
            goto fail;
        }
        char *delim = strchr(split, '\0');
        ww_assert(delim);

        char *name = strndup(ptr, split - ptr);
        check_alloc(name);
        char *value = strndup(split + 1, delim - split + 1);
        check_alloc(value);

        list_envvar_append(&penv, (struct envvar){name, value});

        ptr = delim + 1;
    }

    return penv;

fail:
    penv_destroy(&penv);
    return penv;
}

static bool
read_passthrough_fd() {
    char *passthrough_fd_env = getenv(PASSTHROUGH_FD_ENV);
    if (!passthrough_fd_env) {
        ww_log(LOG_INFO, "no environment passthrough fd");
        return false;
    }
    unsetenv(PASSTHROUGH_FD_ENV);

    int fd = atoi(passthrough_fd_env);
    if (fd <= 0) {
        ww_log(LOG_ERROR, "failed to parse passthrough fd '%s' from env", passthrough_fd_env);
        return true;
    }

    ssize_t len = lseek(fd, 0, SEEK_END);
    if (len == -1) {
        ww_log_errno(LOG_ERROR, "failed to get size of passthrough fd");
        goto fail_seek;
    }

    char *buf = mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (buf == MAP_FAILED) {
        ww_log_errno(LOG_ERROR, "failed to mmap passthrough fd");
        goto fail_mmap;
    }

    passthrough_env = penv_read(buf);
    ww_assert(munmap(buf, len) == 0);
    close(fd);

    return true;

fail_mmap:
fail_seek:
    close(fd);
    return true;
}

void
env_passthrough_set(const char *name, const char *value) {
    if (used_passthrough) {
        return;
    }

    penv_set(&passthrough_env, name, value);
    ww_log(LOG_INFO, "passthrough env: set %s=%s", name, value);
}

void
env_passthrough_unset(const char *name) {
    if (used_passthrough) {
        return;
    }

    penv_unset(&passthrough_env, name);
}

void
env_passthrough_destroy() {
    used_passthrough = true;

    if (passthrough_envlist) {
        envlist_destroy(passthrough_envlist);
    }
    penv_destroy(&passthrough_env);
}

char **
env_passthrough_get() {
    ww_assert(!used_passthrough && !passthrough_envlist);

    used_passthrough = true;
    passthrough_envlist = penv_to_envlist(&passthrough_env);

    return passthrough_envlist;
}

// HACK: PrismLauncher provides several options which modify the environment variables the game
// is started with:
//
//  - "Enable MangoHud"     Modifies LD_PRELOAD to include shared objects from MangoHud
//  - "Use discrete GPU"    Sets environment variables for Nvidia PRIME
//  - "Use Zink"            Sets environment variables to configure Mesa to use Zink
//
// However, since waywall is run as a wrapper command, waywall also gets these environment
// variables. This is probably not what the user intends, and in the case of "Use discrete GPU"
// is often actively harmful, since only the game should use the discrete GPU.
//
// So, this function copies the current environment into a memfd, re-executes waywall with the
// environment of the parent process, and then the restarted waywall process starts the
// Minecraft instance with the environment passed to it via the memfd. This ensures that waywall
// does not inherit any of the environment changes made by PrismLauncher.
//
// If the user doesn't want this behavior, it can be turned off via a flag.
int
env_reexec(char **argv) {
    static constexpr int ENV_SIZE = 1048576;

    // Check for the __WAYWALL_ENV_PASSTHROUGH_FD variable. If present, read it into the passthrough
    // environment and then let execution continue normally since we already restarted.
    if (read_passthrough_fd()) {
        ww_log(LOG_INFO, "skipping env_reexec (got passthrough fd)");
        return 0;
    } else {
        passthrough_env = list_envvar_create();
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
    // variable. This memfd will store all of the environment variables which should be set for
    // the game.
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

    // Read the parent environment into a new list to be passed to execvpe. Add the passthrough fd
    // to this environment so that we don't enter an infinite loop of reexecuting with a fresh
    // environment.
    struct list_envvar penv = penv_read(penvbuf);
    if (!penv.data) {
        goto fail_penv_read;
    }

    char fd_env[16] = {};
    snprintf(fd_env, STATIC_ARRLEN(fd_env), "%d", passthrough_fd);
    penv_set(&penv, PASSTHROUGH_FD_ENV, fd_env);

    // HACK: Portable PrismLauncher modifies several environment variables (each using the
    // colon-delimited format) which must be cleaned up.
    static const char *CLEAN_PORTABLE_VARS[] = {"LD_LIBRARY_PATH", "LD_PRELOAD", "QT_PLUGIN_PATH",
                                                "QT_FONTPATH"};
    static const char *PORTABLE_VARS[] = {"LAUNCHER_LD_LIBRARY_PATH", "LAUNCHER_LD_PRELOAD",
                                          "LAUNCHER_QT_PLUGIN_PATH", "LAUNCHER_QT_FONTPATH"};
    static_assert(STATIC_ARRLEN(CLEAN_PORTABLE_VARS) == STATIC_ARRLEN(PORTABLE_VARS));
    for (size_t i = 0; i < STATIC_ARRLEN(PORTABLE_VARS); i++) {
        char *portable_var = penv_get(&penv, PORTABLE_VARS[i]);
        char *var = penv_get(&penv, CLEAN_PORTABLE_VARS[i]);
        if (!portable_var || !var) {
            continue;
        }

        char *var_orig = var = strdup(var);
        check_alloc(var_orig);

        str cleaned = str_new();
        for (;;) {
            char *next = strchr(var, ':');
            if (next) {
                *next = '\0';
            }

            if (!colon_var_contains(portable_var, var)) {
                str_append(&cleaned, var);
                str_append(&cleaned, ":");
            }

            if (!next) {
                break;
            }
            var = next + 1;
        }
        penv_set(&penv, CLEAN_PORTABLE_VARS[i], cleaned);

        ww_log(LOG_INFO, "cleaned portable variable %s=%s", CLEAN_PORTABLE_VARS[i], cleaned);

        str_free(cleaned);
        free(var_orig);
    }

    char **envlist = penv_to_envlist(&penv);

    ww_log(LOG_INFO, "set passthrough environment fd to %d, restarting", passthrough_fd);
    util_execvpe(argv[0], argv, envlist);

    // There's no need to duplicate the error handling cases. If execvpe failed then it's fine
    // to just run through all of the error handling labels to cleanup.
    envlist_destroy(envlist);
    penv_destroy(&penv);
    ww_log_errno(LOG_ERROR, "env_reexec failed");

fail_penv_read:
fail_write_passthrough:
    close(passthrough_fd);

fail_open_passthrough:
fail_read:
    free(penvbuf);
    close(fd);
    return 1;
}
