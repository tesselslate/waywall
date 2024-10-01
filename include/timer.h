#ifndef WAYWALL_TIMER_H
#define WAYWALL_TIMER_H

#include <sys/time.h>
#include <sys/types.h>

typedef void (*ww_timer_func_t)(void *data);

struct ww_timer {
    struct server *server;

    struct ww_timer_entry {
        int fd;
        struct wl_event_source *src;

        ww_timer_func_t func;
        void *data;
    } *data;
    ssize_t len, cap;
};

struct ww_timer *ww_timer_create(struct server *server);
void ww_timer_destroy(struct ww_timer *timer);

int ww_timer_add_entry(struct ww_timer *timer, struct timespec expiration, ww_timer_func_t func,
                       void *data);

#endif
