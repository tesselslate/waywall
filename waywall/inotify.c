#include "inotify.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdalign.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <wayland-server-core.h>

static int
tick_inotify(int fd, uint32_t mask, void *data) {
    struct inotify *inotify = data;

    char buf[4096] __attribute__((aligned(alignof(struct inotify_event))));
    const struct inotify_event *event;

    for (;;) {
        ssize_t len = read(fd, buf, STATIC_ARRLEN(buf));
        if (len == -1 && errno != EAGAIN) {
            ww_log_errno(LOG_ERROR, "failed to read inotify fd");
            return 0;
        }
        if (len <= 0) {
            return 0;
        }

        for (char *ptr = buf; ptr < buf + len; ptr += sizeof(struct inotify_event) + event->len) {
            event = (const struct inotify_event *)ptr;

            ww_assert(event->wd >= 0 && event->wd <= inotify->len);

            if (!inotify->wd[event->wd].func) {
                if (event->mask & IN_IGNORED) {
                    continue;
                }

                ww_log(LOG_WARN, "received inotify event for NULL listener (wd=%d)", event->wd);
                continue;
            }

            inotify->wd[event->wd].func(event->wd, event->mask, event->name,
                                        inotify->wd[event->wd].data);
        }
    }
}

struct inotify *
inotify_create(struct wl_event_loop *loop) {
    struct inotify *inotify = zalloc(1, sizeof(*inotify));

    inotify->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify->fd == -1) {
        ww_log_errno(LOG_ERROR, "failed to create inotify instance");
        goto fail_fd;
    }

    inotify->src =
        wl_event_loop_add_fd(loop, inotify->fd, WL_EVENT_READABLE, tick_inotify, inotify);
    if (!inotify->src) {
        ww_log(LOG_ERROR, "failed to create inotify event source");
        goto fail_src;
    }

    inotify->wd = zalloc(8, sizeof(*inotify->wd));
    inotify->len = 1; // Watch descriptors are allocated starting at 1.
    inotify->cap = 8;

    return inotify;

fail_src:
    close(inotify->fd);

fail_fd:
    free(inotify);
    return NULL;
}

void
inotify_destroy(struct inotify *inotify) {
    wl_event_source_remove(inotify->src);
    close(inotify->fd);
    if (inotify->wd) {
        free(inotify->wd);
    }
    free(inotify);
}

int
inotify_subscribe(struct inotify *inotify, const char *path, uint32_t mask, inotify_func_t func,
                  void *data) {
    int wd = inotify_add_watch(inotify->fd, path, mask);
    if (wd == -1) {
        ww_log_errno(LOG_ERROR, "failed to add watch (%s, %" PRIu32 ")", path, mask);
        return -1;
    }

    // This assumption will fail if INT_MAX watch descriptors are allocated, which should not
    // happen under any circumstances.
    ww_assert(wd >= inotify->len);

    if (wd >= inotify->cap) {
        ww_assert(inotify->cap < SSIZE_MAX / 2);

        ssize_t cap = inotify->cap * 2;
        void *data = realloc(inotify->wd, cap * sizeof(*inotify->wd));
        check_alloc(data);

        inotify->wd = data;
        inotify->cap = cap;
    }

    inotify->wd[wd].func = func;
    inotify->wd[wd].data = data;
    inotify->len = wd;

    return wd;
}

void
inotify_unsubscribe(struct inotify *inotify, int wd) {
    ww_assert(wd >= 0 && wd <= inotify->len);

    if (inotify_rm_watch(inotify->fd, wd) != 0) {
        ww_log_errno(LOG_ERROR, "failed to remove watch %d", wd);
    }

    inotify->wd[wd].func = NULL;
    inotify->wd[wd].data = NULL;
}
