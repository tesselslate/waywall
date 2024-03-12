#ifndef WAYWALL_WALL_H
#define WAYWALL_WALL_H

#include <sys/types.h>
#include <wayland-server-core.h>

#define MAX_INSTANCES 128

struct wall {
    struct config *cfg;
    struct server *server;
    struct inotify *inotify;

    struct instance *instances[MAX_INSTANCES];
    ssize_t num_instances;

    int active_instance; // -1 on wall

    uint32_t modifiers;
    bool buttons[16];
    bool pointer_locked;
    double mx, my;

    struct wl_listener on_pointer_lock;
    struct wl_listener on_pointer_unlock;
    struct wl_listener on_resize;
    struct wl_listener on_view_create;
    struct wl_listener on_view_destroy;
};

struct wall *wall_create(struct server *server, struct inotify *inotify, struct config *cfg);
void wall_destroy(struct wall *wall);

#endif
