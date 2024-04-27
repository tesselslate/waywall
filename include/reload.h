#ifndef WAYWALL_RELOAD_H
#define WAYWALL_RELOAD_H

#include "util/str.h"
#include <sys/types.h>

struct config;

typedef void (*reload_func_t)(struct config *cfg, void *data);

struct reload {
    struct inotify *inotify;
    const char *profile;

    reload_func_t func;
    void *data;

    str config_path;
    int config_dir_wd;
    struct {
        int *data;
        ssize_t len, cap;
    } config_wd;
};

struct reload *reload_create(struct inotify *inotify, const char *profile, reload_func_t callback, void *data);
void reload_destroy(struct reload *rl);

#endif
