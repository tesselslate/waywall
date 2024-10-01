#include "timer.h"
#include "server/server.h"
#include "util/alloc.h"
#include "util/log.h"
#include <sys/timerfd.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>

static void destroy_entry(struct ww_timer *timer, ssize_t index);

static int
handle_timerfd(int32_t fd, uint32_t mask, void *data) {
    struct ww_timer *timer = data;

    ssize_t entry_index = -1;
    for (ssize_t i = 0; i < timer->len; i++) {
        if (timer->data[i].fd == fd) {
            entry_index = i;
            break;
        }
    }
    ww_assert(entry_index >= 0);

    struct ww_timer_entry *entry = &timer->data[entry_index];
    entry->func(entry->data);

    destroy_entry(timer, entry_index);
    return 0;
}

static struct ww_timer_entry *
alloc_entry(struct ww_timer *timer) {
    if (timer->len < timer->cap) {
        return &timer->data[timer->len++];
    }

    ssize_t cap = timer->cap * 2;
    struct ww_timer_entry *new_data = realloc(timer->data, sizeof(*timer->data) * cap);
    check_alloc(new_data);

    timer->data = new_data;
    timer->cap = cap;
    return &timer->data[timer->len++];
}

static void
destroy_entry(struct ww_timer *timer, ssize_t index) {
    struct ww_timer_entry *entry = &timer->data[index];

    wl_event_source_remove(entry->src);
    close(entry->fd);

    memmove(timer->data + index, timer->data + index + 1,
            sizeof(*timer->data) * (timer->len - index - 1));
    timer->len--;
}

struct ww_timer *
ww_timer_create(struct server *server) {
    struct ww_timer *timer = zalloc(1, sizeof(*timer));
    timer->server = server;

    timer->data = zalloc(8, sizeof(*timer->data));
    timer->cap = 8;

    return timer;
}

void
ww_timer_destroy(struct ww_timer *timer) {
    for (ssize_t i = 0; i < timer->len; i++) {
        struct ww_timer_entry *entry = &timer->data[i];

        wl_event_source_remove(entry->src);
        close(entry->fd);
    }

    free(timer->data);
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

    struct ww_timer_entry *entry = alloc_entry(timer);
    entry->fd = timerfd;
    entry->src = src;

    entry->func = func;
    entry->data = data;

    return 0;

fail_settime:
    close(timerfd);
    return 1;
}
