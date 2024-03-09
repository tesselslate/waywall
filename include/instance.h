#ifndef WAYWALL_INSTANCE_H
#define WAYWALL_INSTANCE_H

#include <stdbool.h>
#include <sys/types.h>

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

    struct server_view *view;
};

struct instance *instance_create(struct server_view *view);
void instance_destroy(struct instance *instance);

#endif
