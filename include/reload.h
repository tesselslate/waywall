#ifndef WAYWALL_RELOAD_H
#define WAYWALL_RELOAD_H

#include "config/config.h"
#include "util/list.h"
#include "util/str.h"
#include <sys/types.h>

typedef void (*reload_func_t)(struct config *cfg, void *data);

struct reload {
    struct inotify *inotify;
    struct ww_timer *timer;
    const char *profile;

    reload_func_t func;
    void *data;

    str config_path;
    int config_dir_wd;
    struct list_int config_wd;

    struct ww_timer_entry *timer_entry;
};

struct reload *reload_create(struct inotify *inotify, struct ww_timer *timer, const char *profile,
                             reload_func_t callback, void *data);
void reload_destroy(struct reload *rl);
void reload_disable(struct reload *rl);

#endif
