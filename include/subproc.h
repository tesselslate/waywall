#pragma once

#include "util/list.h"
#include <sys/types.h>

LIST_DEFINE(struct subproc_entry, list_subproc_entry);

struct subproc {
    struct server *server;
    struct list_subproc_entry entries;
};

struct subproc_entry {
    pid_t pid;
    int pidfd;
    struct wl_event_source *pidfd_src;
};

struct subproc *subproc_create(struct server *server);
void subproc_destroy(struct subproc *subproc);
void subproc_exec(struct subproc *subproc, char *cmd[static 64]);
