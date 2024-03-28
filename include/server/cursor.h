#ifndef WAYWALL_SERVER_CURSOR_H
#define WAYWALL_SERVER_CURSOR_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct server;

struct server_cursor {
    struct server *server;

    struct wl_cursor_theme *theme;
    struct wl_cursor_image *image;
    struct wl_buffer *buffer;

    struct wl_surface *surface;
    uint32_t last_enter;
    bool show;

    struct wl_listener on_pointer_enter;
};

struct server_cursor *server_cursor_create(struct server *server);
void server_cursor_destroy(struct server_cursor *cursor);
void server_cursor_hide(struct server_cursor *cursor);
void server_cursor_show(struct server_cursor *cursor);
int server_cursor_use_theme(struct server_cursor *cursor, const char *name, const char *icon,
                            int size);

#endif
