#ifndef WAYWALL_WRAP_H
#define WAYWALL_WRAP_H

#include "util/box.h"
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct wrap {
    struct config *cfg;

    struct server *server;
    struct server_gl *gl;
    struct scene *scene;

    struct inotify *inotify;
    struct subproc *subproc;
    struct ww_timer *timer;

    int32_t width, height;
    bool is_fullscreen;
    bool allow_mc_x11;

    struct server_view *view;
    struct instance *instance;
    struct {
        int32_t w, h;
    } active_res;

    struct wrap_floating {
        struct wl_list views; // floating_view.link
        bool visible;

        struct floating_view *grab;
        double grab_x, grab_y;

        struct floating_view *anchored;
        struct wl_listener on_anchored_resize;
    } floating;

    struct {
        uint32_t modifiers;
        bool pointer_locked;
        double x, y;
    } input;

    struct wl_listener on_close;
    struct wl_listener on_pointer_lock;
    struct wl_listener on_pointer_unlock;
    struct wl_listener on_resize;
    struct wl_listener on_view_create;
    struct wl_listener on_view_destroy;
};

struct wrap *wrap_create(struct server *server, struct inotify *inotify, struct ww_timer *timer,
                         struct config *cfg, bool allow_mc_x11);
void wrap_destroy(struct wrap *wrap);
int wrap_set_config(struct wrap *wrap, struct config *cfg);

void wrap_lua_exec(struct wrap *wrap, char *cmd[static 64]);
void wrap_lua_press_key(struct wrap *wrap, uint32_t keycode);
int wrap_lua_set_res(struct wrap *wrap, int32_t width, int32_t height);
void wrap_lua_show_floating(struct wrap *wrap, bool show);
void wrap_lua_toggle_fullscreen(struct wrap *wrap);

#endif
