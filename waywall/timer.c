#include "timer.h"
#include "server/server.h"
#include "util/alloc.h"
#include "util/log.h"
#include <sys/timerfd.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>

static int
handle_timerfd(int32_t fd, uint32_t mask, void *data) {
    struct ww_timer_entry *entry = data;
    entry->fire(entry->data);

    return 0;
}

struct ww_timer *
ww_timer_create(struct server *server) {
    struct ww_timer *timer = zalloc(1, sizeof(*timer));
    timer->server = server;

    wl_list_init(&timer->entries);

    return timer;
}

void
ww_timer_destroy(struct ww_timer *timer) {
    struct ww_timer_entry *entry, *tmp;
    wl_list_for_each_safe (entry, tmp, &timer->entries, link) {
        entry->destroy(entry->data);
        ww_timer_entry_destroy(entry);
    }

    free(timer);
}

struct ww_timer_entry *
ww_timer_add_entry(struct ww_timer *timer, struct timespec duration, ww_timer_func_t fire,
                   ww_timer_func_t destroy, void *data) {
    int timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (timerfd == -1) {
        ww_log_errno(LOG_ERROR, "failed to create timerfd");
        return NULL;
    }

    struct itimerspec its = {
        .it_value = duration,
        .it_interval = {0},
    };
    if (timerfd_settime(timerfd, 0, &its, NULL) != 0) {
        ww_log_errno(LOG_ERROR, "failed to set timerfd");
        goto fail_settime;
        return NULL;
    }

    struct ww_timer_entry *entry = zalloc(1, sizeof(*entry));

    entry->src = wl_event_loop_add_fd(wl_display_get_event_loop(timer->server->display), timerfd,
                                      WL_EVENT_READABLE, handle_timerfd, entry);
    check_alloc(entry->src);

    entry->fd = timerfd;
    entry->fire = fire;
    entry->destroy = destroy;
    entry->data = data;

    wl_list_insert(&timer->entries, &entry->link);

    return entry;

fail_settime:
    close(timerfd);
    return NULL;
}

void
ww_timer_entry_destroy(struct ww_timer_entry *entry) {
    wl_event_source_remove(entry->src);
    close(entry->fd);

    wl_list_remove(&entry->link);
    free(entry);
}

int
ww_timer_entry_set_duration(struct ww_timer_entry *entry, struct timespec duration) {
    struct itimerspec its = {
        .it_value = duration,
        .it_interval = {0},
    };

    if (timerfd_settime(entry->fd, 0, &its, NULL) != 0) {
        ww_log_errno(LOG_ERROR, "failed to set timerfd");
        return -1;
    }

    return 0;
}
