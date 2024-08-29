#ifndef WAYWALL_SUBPROC_H
#define WAYWALL_SUBPROC_H

#include <sys/types.h>

struct subproc {
    struct server *server;

    struct subproc_entry {
        pid_t pid;
        int pidfd;
        struct wl_event_source *pidfd_src;
    } *data;
    ssize_t len, cap;
};

struct subproc *subproc_create(struct server *server);
void subproc_destroy(struct subproc *subproc);
void subproc_exec(struct subproc *subproc, char *cmd[static 64]);

#endif
