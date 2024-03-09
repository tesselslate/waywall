#ifndef WAYWALL_INOTIFY_H
#define WAYWALL_INOTIFY_H

#include <stdint.h>
#include <sys/types.h>

struct wl_event_loop;

typedef void (*inotify_func_t)(int wd, uint32_t mask, void *data);

struct inotify {
    struct wl_event_source *src;
    int fd;

    struct {
        inotify_func_t func;
        void *data;
    } *wd;
    ssize_t len, cap;
};

struct inotify *inotify_create(struct wl_event_loop *loop);
void inotify_destroy(struct inotify *inotify);
int inotify_subscribe(struct inotify *inotify, const char *path, uint32_t mask, inotify_func_t func,
                      void *data);
void inotify_unsubscribe(struct inotify *inotify, int wd);

#endif
