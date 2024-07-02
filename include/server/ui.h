#ifndef WAYWALL_SERVER_UI_H
#define WAYWALL_SERVER_UI_H

#include "config/config.h"
#include "util/box.h"
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <wayland-server-core.h>
#include <wayland-util.h>

struct server_ui {
    struct server *server;

    struct server_ui_config *config;

    struct wl_region *empty_region;

    struct {
        struct wl_surface *surface;
        struct wp_viewport *viewport;
    } root;
    struct {
        struct wl_buffer *buffer;
        struct wl_surface *surface;
        struct wl_subsurface *subsurface;
    } tree;

    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct zxdg_toplevel_decoration_v1 *xdg_decoration;

    int32_t width, height;
    bool mapped, resize;

    struct wl_list views; // server_view.link

    struct server_txn *inflight_txn;

    struct {
        struct wl_signal close;        // data: NULL
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

    struct {
        uint32_t x, y;
        struct box crop;
    } state;

    const struct server_view_impl *impl;
    void *impl_data;

    struct wl_listener on_surface_commit;

    struct {
        struct wl_signal destroy; // data: NULL
    } events;
};

struct server_view_impl {
    const char *name;

    void (*close)(void *impl_data);
    pid_t (*get_pid)(void *impl_data);
    char *(*get_title)(void *impl_data);
    void (*set_size)(void *impl_data, uint32_t width, uint32_t height);
};

struct server_txn {
    struct server_ui *ui;
    struct wl_event_source *timer;

    bool applied; // whether server_txn_apply has been called

    struct wl_list views;        // server_txn_view.link
    struct wl_list dependencies; // server_txn_dep.link
};

struct server_txn_view {
    struct server_txn *parent;

    struct wl_list link; // server_txn.views
    struct server_view *view;

    struct server_txn_dep {
        struct wl_list link; // server_txn.dependencies
        struct wl_listener listener;
    } resize_dep;

    uint32_t x, y, width, height, dest_width, dest_height;
    struct box crop;
    struct wl_surface *above;
    bool visible;

    enum server_txn_view_state {
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
void server_ui_apply(struct server_ui *ui, struct server_txn *txn);
void server_ui_hide(struct server_ui *ui);
void server_ui_show(struct server_ui *ui);
void server_ui_use_config(struct server_ui *ui, struct server_ui_config *config);

struct server_ui_config *server_ui_config_create(struct server_ui *ui, struct config *cfg);
void server_ui_config_destroy(struct server_ui_config *config);

void server_view_close(struct server_view *view);
pid_t server_view_get_pid(struct server_view *view);
char *server_view_get_title(struct server_view *view);

struct server_view *server_view_create(struct server_ui *ui, struct server_surface *surface,
                                       const struct server_view_impl *impl, void *impl_data);
void server_view_destroy(struct server_view *view);

struct server_txn *server_txn_create();
struct server_txn_view *server_txn_get_view(struct server_txn *txn, struct server_view *view);
void server_txn_view_set_above(struct server_txn_view *view, struct wl_surface *surface);
void server_txn_view_set_crop(struct server_txn_view *view, int32_t x, int32_t y, int32_t width,
                              int32_t height);
void server_txn_view_set_dest_size(struct server_txn_view *view, uint32_t width, uint32_t height);
void server_txn_view_set_pos(struct server_txn_view *view, uint32_t x, uint32_t y);
void server_txn_view_set_size(struct server_txn_view *view, uint32_t width, uint32_t height);
void server_txn_view_set_visible(struct server_txn_view *view, bool visible);

struct ui_rectangle *ui_rectangle_create(struct server_ui *ui, uint32_t x, uint32_t y,
                                         uint32_t width, uint32_t height,
                                         const uint8_t rgba[static 4]);
void ui_rectangle_destroy(struct ui_rectangle *rect);
void ui_rectangle_set_visible(struct ui_rectangle *rect, bool visible);

#endif
