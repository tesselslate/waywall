#include "timer.h"
#include "server/server.h"
#include "util/alloc.h"
#include "util/list.h"
#include "util/log.h"
#include <sys/timerfd.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>

LIST_DEFINE_IMPL(struct ww_timer_entry, list_timer_entry);

static void destroy_entry(struct ww_timer *timer, ssize_t index);

static int
handle_timerfd(int32_t fd, uint32_t mask, void *data) {
    struct ww_timer *timer = data;

    ssize_t entry_index = -1;
    for (ssize_t i = 0; i < timer->entries.len; i++) {
        if (timer->entries.data[i].fd == fd) {
            entry_index = i;
            break;
        }
    }
    ww_assert(entry_index >= 0);

    struct ww_timer_entry *entry = &timer->entries.data[entry_index];
    entry->func(entry->data);

    destroy_entry(timer, entry_index);
    return 0;
}

static void
destroy_entry(struct ww_timer *timer, ssize_t index) {
    struct ww_timer_entry *entry = &timer->entries.data[index];

    wl_event_source_remove(entry->src);
    close(entry->fd);

    list_timer_entry_remove(&timer->entries, index);
}

struct ww_timer *
ww_timer_create(struct server *server) {
    struct ww_timer *timer = zalloc(1, sizeof(*timer));
    timer->server = server;
    timer->entries = list_timer_entry_create();
    return timer;
}

void
ww_timer_destroy(struct ww_timer *timer) {
    for (ssize_t i = 0; i < timer->entries.len; i++) {
        struct ww_timer_entry *entry = &timer->entries.data[i];

        wl_event_source_remove(entry->src);
        close(entry->fd);
    }

    list_timer_entry_destroy(&timer->entries);
    free(timer);
}

int
ww_timer_add_entry(struct ww_timer *timer, struct timespec duration, ww_timer_func_t func,
                   void *data) {
    int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (timerfd == -1) {
        ww_log_errno(LOG_ERROR, "failed to create timerfd");
        return 1;
    }

    struct itimerspec its = {
        .it_value = duration,
        .it_interval = {0},
    };
    if (timerfd_settime(timerfd, 0, &its, NULL) != 0) {
        ww_log_errno(LOG_ERROR, "failed to set timerfd");
        goto fail_settime;
        return 1;
    }

    struct wl_event_source *src =
        wl_event_loop_add_fd(wl_display_get_event_loop(timer->server->display), timerfd,
                             WL_EVENT_READABLE, handle_timerfd, timer);

    struct ww_timer_entry entry = {0};

    entry.fd = timerfd;
    entry.src = src;
    entry.func = func;
    entry.data = data;

    list_timer_entry_append(&timer->entries, entry);

    return 0;

fail_settime:
    close(timerfd);
    return 1;
}
