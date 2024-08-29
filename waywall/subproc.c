#include "subproc.h"
#include "server/server.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/syscall.h"
#include <fcntl.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>

static void destroy_entry(struct subproc *subproc, ssize_t index);

static int
handle_pidfd(int32_t fd, uint32_t mask, void *data) {
    struct subproc *subproc = data;

    ssize_t entry_index = -1;
    for (ssize_t i = 0; i < subproc->len; i++) {
        if (subproc->data[i].pidfd == fd) {
            entry_index = i;
            break;
        }
    }
    ww_assert(entry_index >= 0);

    struct subproc_entry *entry = &subproc->data[entry_index];
    if (waitpid(entry->pid, NULL, 0) != entry->pid) {
        ww_log_errno(LOG_ERROR, "failed to waitpid on child process %jd", (intmax_t)entry->pid);
    }
    if (pidfd_send_signal(entry->pidfd, SIGKILL, NULL, 0) != 0) {
        if (errno != ESRCH) {
            ww_log_errno(LOG_ERROR, "failed to kill child process %jd", (intmax_t)entry->pid);
        }
    }

    destroy_entry(subproc, entry_index);
    return 0;
}

static struct subproc_entry *
alloc_entry(struct subproc *subproc) {
    if (subproc->len < subproc->cap) {
        return &subproc->data[subproc->len++];
    }

    ssize_t cap = subproc->cap * 2;
    struct subproc_entry *new_data = realloc(subproc->data, sizeof(*subproc->data) * cap);
    check_alloc(new_data);

    subproc->data = new_data;
    subproc->cap = cap;
    return &subproc->data[subproc->len++];
}

static void
destroy_entry(struct subproc *subproc, ssize_t index) {
    struct subproc_entry *entry = &subproc->data[index];

    wl_event_source_remove(entry->pidfd_src);
    close(entry->pidfd);

    memmove(subproc->data + index, subproc->data + index + 1,
            sizeof(*subproc->data) - (subproc->len - index - 1));
    subproc->len--;
}

struct subproc *
subproc_create(struct server *server) {
    struct subproc *subproc = zalloc(1, sizeof(*subproc));
    subproc->server = server;

    subproc->data = zalloc(8, sizeof(*subproc->data));
    subproc->cap = 8;

    return subproc;
}

void
subproc_destroy(struct subproc *subproc) {
    for (ssize_t i = 0; i < subproc->len; i++) {
        struct subproc_entry *entry = &subproc->data[i];

        if (pidfd_send_signal(entry->pidfd, SIGKILL, NULL, 0) != 0) {
            if (errno != ESRCH) {
                ww_log_errno(LOG_ERROR, "failed to kill child process %jd", (intmax_t)entry->pid);
            }
        }

        wl_event_source_remove(entry->pidfd_src);
        close(entry->pidfd);
    }

    free(subproc->data);
    free(subproc);
}

void
subproc_exec(struct subproc *subproc, char *cmd[static 64]) {
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        int out = open("/dev/null", O_WRONLY);
        if (out == -1) {
            ww_log_errno(LOG_ERROR, "failed to open /dev/null in child process");
            exit(EXIT_FAILURE);
        }
        if (dup2(out, STDOUT_FILENO) == -1) {
            ww_log_errno(LOG_ERROR, "failed to duplicate /dev/null to stdout in child process");
            exit(EXIT_FAILURE);
        }

        execvp(cmd[0], cmd);
        ww_log_errno(LOG_ERROR, "failed to execvp() in child porcess");
        exit(EXIT_FAILURE);
    } else if (pid == -1) {
        // Parent process (error)
        ww_log_errno(LOG_ERROR, "failed to fork() child process");
        return;
    }

    int pidfd = pidfd_open(pid, 0);
    if (pidfd == -1) {
        ww_log_errno(LOG_ERROR, "failed to open pidfd for subprocess %jd", (intmax_t)pid);
        return;
    }

    struct wl_event_source *src =
        wl_event_loop_add_fd(wl_display_get_event_loop(subproc->server->display), pidfd,
                             WL_EVENT_READABLE, handle_pidfd, subproc);
    check_alloc(src);

    struct subproc_entry *entry = alloc_entry(subproc);
    entry->pid = pid;
    entry->pidfd = pidfd;
    entry->pidfd_src = src;
}
