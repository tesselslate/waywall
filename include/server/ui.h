#ifndef WAYWALL_SERVER_UI_H
#define WAYWALL_SERVER_UI_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct server_ui {
    struct server *server;

    struct wl_buffer *background;
    struct wl_surface *surface;
    struct wp_viewport *viewport;

    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;

    int32_t width, height;
    bool mapped;

    struct wl_list views;

    struct {
        struct wl_signal view_create;  // data: struct server_view *
        struct wl_signal view_destroy; // data: struct server_view *
    } events;
};

struct server_view {
    struct server_ui *ui;
    struct wl_list link; // server_ui.views

    struct server_surface *surface;

    struct wl_subsurface *subsurface;
    struct wp_viewport *viewport;

    int32_t x, y;

    const struct server_view_impl *impl;
    struct wl_resource *impl_resource;
};

struct server_view_impl {
    const char *name;

    void (*set_size)(struct wl_resource *impl_resource, uint32_t width, uint32_t height);
};

void server_ui_destroy(struct server_ui *ui);
int server_ui_init(struct server *server, struct server_ui *ui);
void server_ui_hide(struct server_ui *ui);
void server_ui_show(struct server_ui *ui);

void server_view_set_crop(struct server_view *view, double x, double y, double width,
                          double height);
void server_view_set_dest_size(struct server_view *view, uint32_t width, uint32_t height);
void server_view_set_position(struct server_view *view, int32_t x, int32_t y);
void server_view_set_size(struct server_view *view, uint32_t width, uint32_t height);
void server_view_unset_crop(struct server_view *view);

struct server_view *server_view_create(struct server_ui *ui, struct server_surface *surface,
                                       const struct server_view_impl *impl,
                                       struct wl_resource *impl_resource);
void server_view_destroy(struct server_view *view);

#endif
