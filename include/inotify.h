#pragma once

#include "util/list.h"
#include <stdint.h>
#include <sys/types.h>
#include <wayland-server-core.h>

LIST_DEFINE(struct inotify_entry, list_inotify_entry);

typedef void (*inotify_func_t)(int wd, uint32_t mask, const char *name, void *data);

struct inotify {
    struct wl_event_source *src;
    int fd;

    struct list_inotify_entry entries;
};

struct inotify_entry {
    inotify_func_t func;
    void *data;
};

struct inotify *inotify_create(struct wl_event_loop *loop);
void inotify_destroy(struct inotify *inotify);
int inotify_subscribe(struct inotify *inotify, const char *path, uint32_t mask, inotify_func_t func,
                      void *data);
void inotify_unsubscribe(struct inotify *inotify, int wd);
