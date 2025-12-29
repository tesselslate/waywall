#pragma once

#include "inotify.h"
#include "util/str.h"
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

struct instance {
    char *dir;
    pid_t pid;

    int state_wd, state_fd;
    struct instance_state {
        enum {
            SCREEN_TITLE,
            SCREEN_WAITING,
            SCREEN_GENERATING,
            SCREEN_PREVIEWING,
            SCREEN_INWORLD,
            SCREEN_WALL,
        } screen;
        union {
            int percent;
            enum {
                INWORLD_UNPAUSED,
                INWORLD_PAUSED,
                INWORLD_MENU,
            } inworld;
        } data;
    } state;

    struct server_view *view;
};

struct instance *instance_create(struct server_view *view, struct inotify *inotify);
void instance_destroy(struct instance *instance);
str instance_get_state_path(struct instance *instance);
void instance_state_update(struct instance *instance);
