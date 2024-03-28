#ifndef WAYWALL_WALL_H
#define WAYWALL_WALL_H

#include <sys/types.h>
#include <wayland-server-core.h>

#define MAX_INSTANCES 128

struct wall {
    struct config *cfg;
    struct server *server;
    struct inotify *inotify;

    struct counter *counter;
    struct cpu_manager *cpu;

    struct instance *instances[MAX_INSTANCES];
    ssize_t num_instances;

    struct config_layout *layout;
    struct {
        struct ui_rectangle **data;
        ssize_t len, cap;
    } layout_rectangles;
    int last_hovered;
    int32_t width, height;

    struct {
        int32_t w, h;
    } active_res;
    int active_instance; // -1 on wall

    struct {
        uint32_t modifiers;
        bool buttons[16];
        bool pointer_locked;
        double mx, my;
    } input;

    struct wl_listener on_pointer_lock;
    struct wl_listener on_pointer_unlock;
    struct wl_listener on_resize;
    struct wl_listener on_view_create;
    struct wl_listener on_view_destroy;
};

struct wall *wall_create(struct server *server, struct inotify *inotify, struct config *cfg);
void wall_destroy(struct wall *wall);
int wall_set_config(struct wall *wall, struct config *cfg);

int wall_lua_get_hovered(struct wall *wall);
int wall_lua_play(struct wall *wall, int id);
int wall_lua_reset_one(struct wall *wall, int id);
int wall_lua_reset_many(struct wall *wall, size_t num_ids, int ids[static num_ids]);
int wall_lua_return(struct wall *wall);
int wall_lua_set_res(struct wall *wall, int id, int32_t width, int32_t height);

#endif
