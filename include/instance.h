#ifndef WAYWALL_INSTANCE_H
#define WAYWALL_INSTANCE_H

#include <stdbool.h>
#include <sys/time.h>
#include <sys/types.h>

struct inotify;
struct server_view;

struct instance {
    char *dir;
    pid_t pid;

    struct instance_mods {
        bool atum : 1;
        bool standard_settings : 1;
        bool state_output : 1;
        bool world_preview : 1;
    } mods;
    int version;

    int state_wd, state_fd;
    struct instance_state {
        enum {
            SCREEN_TITLE,
            SCREEN_WAITING,
            SCREEN_GENERATING,
            SCREEN_PREVIEWING,
            SCREEN_INWORLD,
        } screen;
        union {
            int percent;
            enum {
                INWORLD_UNPAUSED,
                INWORLD_PAUSED,
                INWORLD_MENU,
            } inworld;
        } data;

        struct timespec last_preview;
    } state;

    struct server_view *view;
};

struct instance *instance_create(struct server_view *view, struct inotify *inotify);
void instance_destroy(struct instance *instance);
char *instance_get_state_path(struct instance *instance);
bool instance_reset(struct instance *instance);
void instance_state_update(struct instance *instance);

#endif
