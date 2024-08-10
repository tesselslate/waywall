#ifndef WAYWALL_WRAP_H
#define WAYWALL_WRAP_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct wrap {
    struct config *cfg;
    struct server *server;

    int32_t width, height;
    struct server_view *view;

    struct {
        int32_t w, h;
    } active_res;

    struct {
        uint32_t modifiers;
        bool pointer_locked;
    } input;

    struct wl_listener on_close;
    struct wl_listener on_pointer_lock;
    struct wl_listener on_pointer_unlock;
    struct wl_listener on_resize;
    struct wl_listener on_view_create;
    struct wl_listener on_view_destroy;
};

struct wrap *wrap_create(struct server *server, struct config *cfg);
void wrap_destroy(struct wrap *wrap);
int wrap_set_config(struct wrap *wrap, struct config *cfg);

void wrap_lua_press_key(struct wrap *wrap, uint32_t keycode);
int wrap_lua_set_res(struct wrap *wrap, int32_t width, int32_t height);

#endif
