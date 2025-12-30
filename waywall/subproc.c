#include "subproc.h"
#include "server/server.h"
#include "util/alloc.h"
#include "util/list.h"
#include "util/log.h"
#include "util/syscall.h"
#include <fcntl.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>

LIST_DEFINE_IMPL(struct subproc_entry, list_subproc_entry);

static void destroy_entry(struct subproc *subproc, ssize_t index);

static int
handle_pidfd(int32_t fd, uint32_t mask, void *data) {
    struct subproc *subproc = data;

    ssize_t entry_index = -1;
    for (ssize_t i = 0; i < subproc->entries.len; i++) {
        if (subproc->entries.data[i].pidfd == fd) {
            entry_index = i;
            break;
        }
    }
    ww_assert(entry_index >= 0);

    struct subproc_entry *entry = &subproc->entries.data[entry_index];
    if (waitpid(entry->pid, nullptr, 0) != entry->pid) {
        ww_log_errno(LOG_ERROR, "failed to waitpid on child process %jd", (intmax_t)entry->pid);
    }
    if (pidfd_send_signal(entry->pidfd, SIGKILL, nullptr, 0) != 0) {
        if (errno != ESRCH) {
            ww_log_errno(LOG_ERROR, "failed to kill child process %jd", (intmax_t)entry->pid);
        }
    }

    destroy_entry(subproc, entry_index);
    return 0;
}

static void
destroy_entry(struct subproc *subproc, ssize_t index) {
    struct subproc_entry *entry = &subproc->entries.data[index];

    wl_event_source_remove(entry->pidfd_src);
    close(entry->pidfd);

    list_subproc_entry_remove(&subproc->entries, index);
}

struct subproc *
subproc_create(struct server *server) {
    struct subproc *subproc = zalloc(1, sizeof(*subproc));
    subproc->server = server;
    subproc->entries = list_subproc_entry_create();
    return subproc;
}

void
subproc_destroy(struct subproc *subproc) {
    for (ssize_t i = 0; i < subproc->entries.len; i++) {
        struct subproc_entry *entry = &subproc->entries.data[i];

        if (pidfd_send_signal(entry->pidfd, SIGKILL, nullptr, 0) != 0) {
            if (errno != ESRCH) {
                ww_log_errno(LOG_ERROR, "failed to kill child process %jd", (intmax_t)entry->pid);
            }
        }

        wl_event_source_remove(entry->pidfd_src);
        close(entry->pidfd);
    }

    list_subproc_entry_destroy(&subproc->entries);
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

    struct subproc_entry entry = {};

    entry.pid = pid;
    entry.pidfd = pidfd;
    entry.pidfd_src = src;

    list_subproc_entry_append(&subproc->entries, entry);
}
