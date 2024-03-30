#ifndef WAYWALL_SERVER_WL_DATA_DEVICE_MANAGER_H
#define WAYWALL_SERVER_WL_DATA_DEVICE_MANAGER_H

#include <wayland-server-core.h>

struct server;

struct server_data_device_manager {
    struct wl_global *global;

    struct {
        struct wl_data_device_manager *manager;
    } remote;

    struct {
        struct server_data_source *source;
    } selection;

    struct wl_list devices; // server_data_device.link

    struct wl_listener on_input_focus;
    struct server_view *input_focus;

    struct wl_listener on_display_destroy;
};

struct server_data_device {
    struct server_data_device_manager *parent;
    struct wl_resource *resource;

    struct wl_list link; // server_data_device_manager.devices
};

struct server_data_offer {
    struct server_data_device *device;
    struct server_data_source *source;
    struct wl_resource *resource;

    struct wl_list link; // server_data_source.offers
};

struct server_data_source {
    struct server_data_device_manager *parent;
    struct wl_resource *resource;

    struct wl_list mime_types; // mime_type.link
    bool prepared;

    struct wl_list offers; // server_data_offer.link
};

struct server_data_device_manager *server_data_device_manager_create(struct server *server);

#endif
