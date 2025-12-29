#pragma once

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

        struct wp_tearing_control_v1 *tearing_control;
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
    bool mapped, resize, fullscreen;

    struct wl_list views; // server_view.link

    struct {
        struct wl_signal close;        // data: NULL
        struct wl_signal resize;       // data: NULL
        struct wl_signal view_create;  // data: struct server_view *
        struct wl_signal view_destroy; // data: struct server_view *
    } events;
};

struct server_ui_config {
    struct wl_buffer *background;
    bool tearing;

    uint32_t ninb_opacity;
};

struct server_view {
    struct server_ui *ui;
    struct wl_list link; // server_ui.views

    struct server_surface *surface;

    struct wp_alpha_modifier_surface_v1 *alpha_surface;
    struct wl_subsurface *subsurface;
    struct wp_viewport *viewport;

    struct server_view_state {
        uint32_t x, y;
        uint32_t width, height;
        bool centered;
        bool visible;

        enum {
            VIEW_STATE_POS = (1 << 0),
            VIEW_STATE_SIZE = (1 << 1),
            VIEW_STATE_CENTERED = (1 << 2),
            VIEW_STATE_VISIBLE = (1 << 3),
        } present;
    } current, pending;

    const struct server_view_impl *impl;
    void *impl_data;

    struct wl_listener on_surface_commit;

    struct {
        struct wl_signal destroy; // data: NULL
        struct wl_signal resize;  // data: NULL
    } events;
};

struct server_view_impl {
    const char *name;

    void (*close)(void *impl_data);
    pid_t (*get_pid)(void *impl_data);
    char *(*get_title)(void *impl_data);
    void (*set_size)(void *impl_data, uint32_t width, uint32_t height);
};

struct server_ui *server_ui_create(struct server *server, struct config *cfg);
void server_ui_destroy(struct server_ui *ui);
void server_ui_hide(struct server_ui *ui);
void server_ui_set_fullscreen(struct server_ui *ui, bool fullscreen);
void server_ui_show(struct server_ui *ui);
void server_ui_use_config(struct server_ui *ui, struct server_ui_config *config);

struct server_ui_config *server_ui_config_create(struct server_ui *ui, struct config *cfg);
void server_ui_config_destroy(struct server_ui_config *config);

void server_view_close(struct server_view *view);
pid_t server_view_get_pid(struct server_view *view);
char *server_view_get_title(struct server_view *view);

void server_view_commit(struct server_view *view);
void server_view_refresh(struct server_view *view);
void server_view_set_centered(struct server_view *view, bool centered);
void server_view_set_pos(struct server_view *view, uint32_t x, uint32_t y);
void server_view_set_size(struct server_view *view, uint32_t width, uint32_t height);
void server_view_set_visible(struct server_view *view, bool visible);

struct server_view *server_view_create(struct server_ui *ui, struct server_surface *surface,
                                       const struct server_view_impl *impl, void *impl_data);
void server_view_destroy(struct server_view *view);
