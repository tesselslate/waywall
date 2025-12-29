#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-util.h>

struct server_data_device_manager {
    struct wl_global *global;

    struct server *server;

    struct {
        struct wl_data_device_manager *manager;
        struct wl_data_device *device;
        struct wl_data_source *source;

        struct remote_offer *dnd_offer;
        struct remote_offer *pending_offers[8];
    } remote;

    struct server_selection {
        enum server_selection_type {
            SELECTION_NONE,
            SELECTION_LOCAL,
            SELECTION_REMOTE,
        } type;
        union {
            void *none;
            struct server_data_source *local;
            struct remote_offer *remote;
        } data;
        uint64_t serial;
    } selection;

    struct server_selection_content {
        struct wl_event_source *src_pipe;

        int32_t fd;
        char *data;
        ssize_t len;
    } selection_content;

    struct wl_list devices; // server_data_device.link

    struct wl_listener on_input_focus;
    struct server_view *input_focus;

    struct wl_listener on_keyboard_leave;

    struct wl_listener on_display_destroy;
};

struct server_data_device {
    struct server_data_device_manager *parent;
    struct wl_resource *resource;

    struct wl_list link; // server_data_device_manager.devices
};

struct server_data_offer {
    struct server_data_device *parent;
    struct wl_resource *resource;

    struct server_selection selection;
};

struct server_data_source {
    struct server_data_device_manager *parent;
    struct wl_resource *resource;

    struct wl_list mime_types; // mime_type.link
    bool prepared;
};

struct server_data_device_manager *server_data_device_manager_create(struct server *server);
