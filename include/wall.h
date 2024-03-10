#ifndef WAYWALL_WALL_H
#define WAYWALL_WALL_H

#include <sys/types.h>
#include <wayland-server-core.h>

#define MAX_INSTANCES 128

struct wall {
    struct server *server;
    struct inotify *inotify;

    struct instance *instances[MAX_INSTANCES];
    ssize_t num_instances;

    int active_instance; // -1 on wall

    bool buttons[16];
    double mx, my;

    struct wl_listener on_resize;
    struct wl_listener on_view_create;
    struct wl_listener on_view_destroy;
};

struct wall *wall_create(struct server *server, struct inotify *inotify);
void wall_destroy(struct wall *wall);

#endif
