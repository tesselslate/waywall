#pragma once

#include <sys/time.h>
#include <sys/types.h>
#include <wayland-server-core.h>

typedef void (*ww_timer_func_t)(void *data);

struct ww_timer {
    struct server *server;
    struct wl_list entries; // ww_timer_entry.link
};

struct ww_timer_entry {
    struct wl_list link; // ww_timer.entries

    int fd;
    struct wl_event_source *src;

    ww_timer_func_t fire, destroy;
    void *data;
};

struct ww_timer *ww_timer_create(struct server *server);
void ww_timer_destroy(struct ww_timer *timer);

struct ww_timer_entry *ww_timer_add_entry(struct ww_timer *timer, struct timespec duration,
                                          ww_timer_func_t fire, ww_timer_func_t destroy,
                                          void *data);
void ww_timer_entry_destroy(struct ww_timer_entry *entry);
int ww_timer_entry_set_duration(struct ww_timer_entry *entry, struct timespec duration);
