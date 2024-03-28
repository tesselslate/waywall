#ifndef WAYWALL_SERVER_UI_H
#define WAYWALL_SERVER_UI_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct config;

struct server_ui {
    struct server *server;

    struct server_ui_config *config;

    struct wl_region *empty_region;

    struct wl_surface *surface;
    struct wp_viewport *viewport;

    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;

    int32_t width, height;
    bool mapped;

    struct wl_list views; // server_view.link

    struct {
        struct wl_signal resize;       // data: NULL
        struct wl_signal view_create;  // data: struct server_view *
        struct wl_signal view_destroy; // data: struct server_view *
    } events;
};

struct server_ui_config {
    struct wl_buffer *background;
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

    struct {
        struct wl_signal destroy; // data: NULL
    } events;
};

struct server_view_impl {
    const char *name;

    pid_t (*get_pid)(struct wl_resource *impl_resource);
    char *(*get_title)(struct wl_resource *impl_resource);
    void (*set_size)(struct wl_resource *impl_resource, uint32_t width, uint32_t height);
};

struct transaction {
    struct wl_list views; // transaction_view.link
};

struct transaction_view {
    struct wl_list link; // transaction.views
    struct server_view *view;

    uint32_t x, y, width, height, dest_width, dest_height;
    struct {
        uint32_t x, y, width, height;
    } crop;
    struct wl_surface *above;
    bool visible;

    enum transaction_view_state {
        TXN_VIEW_ABOVE = (1 << 0),
        TXN_VIEW_CROP = (1 << 1),
        TXN_VIEW_DEST_SIZE = (1 << 2),
        TXN_VIEW_POS = (1 << 3),
        TXN_VIEW_SIZE = (1 << 4),
        TXN_VIEW_VISIBLE = (1 << 5),
    } apply;
};

struct ui_rectangle {
    struct server_ui *parent;

    uint32_t x, y;

    struct wl_buffer *buffer;
    struct wl_surface *surface;
    struct wl_subsurface *subsurface;
    struct wp_viewport *viewport;
};

struct server_ui *server_ui_create(struct server *server, struct config *cfg);
void server_ui_destroy(struct server_ui *ui);
void server_ui_hide(struct server_ui *ui);
void server_ui_use_config(struct server_ui *ui, struct server_ui_config *config);
void server_ui_show(struct server_ui *ui);

struct server_ui_config *server_ui_config_create(struct server_ui *ui, struct config *cfg);
void server_ui_config_destroy(struct server_ui_config *config);

pid_t server_view_get_pid(struct server_view *view);
char *server_view_get_title(struct server_view *view);

struct server_view *server_view_create(struct server_ui *ui, struct server_surface *surface,
                                       const struct server_view_impl *impl,
                                       struct wl_resource *impl_resource);
void server_view_destroy(struct server_view *view);

void transaction_apply(struct server_ui *ui, struct transaction *txn);
struct transaction *transaction_create();
struct transaction_view *transaction_get_view(struct transaction *txn, struct server_view *view);
void transaction_destroy(struct transaction *txn);
void transaction_view_set_above(struct transaction_view *view, struct wl_surface *surface);
void transaction_view_set_crop(struct transaction_view *view, uint32_t x, uint32_t y,
                               uint32_t width, uint32_t height);
void transaction_view_set_dest_size(struct transaction_view *view, uint32_t width, uint32_t height);
void transaction_view_set_position(struct transaction_view *view, uint32_t x, uint32_t y);
void transaction_view_set_size(struct transaction_view *view, uint32_t width, uint32_t height);
void transaction_view_set_visible(struct transaction_view *view, bool visible);

struct ui_rectangle *ui_rectangle_create(struct server_ui *ui, uint32_t x, uint32_t y,
                                         uint32_t width, uint32_t height,
                                         const uint8_t rgba[static 4]);
void ui_rectangle_destroy(struct ui_rectangle *rect);
void ui_rectangle_set_visible(struct ui_rectangle *rect, bool visible);

#endif
