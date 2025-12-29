#pragma once

#include "config/config.h"
#include "cursor-shape-v1-client-protocol.h"
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct server_cursor {
    struct server *server;

    struct server_cursor_config *config;
    struct wl_surface *surface;
    uint32_t last_enter;
    bool show;

    struct wp_cursor_shape_device_v1 *shape_device;

    struct wl_listener on_pointer;
    struct wl_listener on_pointer_enter;
};

struct server_cursor_config {
    enum {
        SERVER_CURSOR_CONFIG_TYPE_XCURSOR,
        SERVER_CURSOR_CONFIG_TYPE_SHAPE,
    } type;
    union {
        struct {
            struct wl_cursor_theme *theme;
            struct wl_cursor_image *image;
            struct wl_buffer *buffer;
        } xcursor;
        enum wp_cursor_shape_device_v1_shape shape;
    } data;
};

struct server_cursor *server_cursor_create(struct server *server, struct config *cfg);
void server_cursor_destroy(struct server_cursor *cursor);
void server_cursor_hide(struct server_cursor *cursor);
void server_cursor_show(struct server_cursor *cursor);
void server_cursor_use_config(struct server_cursor *cursor, struct server_cursor_config *config);

struct server_cursor_config *server_cursor_config_create(struct server_cursor *cursor,
                                                         struct config *cfg);
void server_cursor_config_destroy(struct server_cursor_config *config);
