#ifndef WAYWALL_SERVER_SERVER_H
#define WAYWALL_SERVER_SERVER_H

#include "server/ui.h"
#include <wayland-client-core.h>
#include <wayland-server-core.h>

struct server_backend {
    struct wl_display *display;
    struct wl_registry *registry;

    struct {
        struct wl_list names; // seat_name.link

        struct wl_seat *remote;
        uint32_t caps;

        struct wl_keyboard *keyboard;
        struct wl_pointer *pointer;
    } seat;
    struct wl_array shm_formats; // data: uint32_t

    struct wl_compositor *compositor;
    struct zwp_linux_dmabuf_v1 *linux_dmabuf;
    struct zwp_pointer_constraints_v1 *pointer_constraints;
    struct zwp_relative_pointer_manager_v1 *relative_pointer_manager;
    struct wl_shm *shm;
    struct wl_subcompositor *subcompositor;
    struct wp_viewporter *viewporter;
    struct xdg_wm_base *xdg_wm_base;

    struct {
        struct wl_signal seat_keyboard; // data: NULL
        struct wl_signal seat_pointer;  // data: NULL
        struct wl_signal shm_format;    // data: uint32_t *
    } events;
};

struct server {
    struct config *cfg;
    struct wl_display *display;
    struct server_backend backend;
    struct server_ui ui;

    struct server_view *input_focus;
    struct wl_listener on_view_destroy;

    struct server_cursor *cursor;

    struct wl_event_source *backend_source;

    struct remote_buffer_manager *remote_buf;

    struct server_compositor_g *compositor;
    struct server_linux_dmabuf_g *linux_dmabuf;
    struct server_pointer_constraints_g *pointer_constraints;
    struct server_relative_pointer_g *relative_pointer;
    struct server_seat_g *seat;
    struct server_shm_g *shm;
    struct server_xdg_decoration_manager_g *xdg_decoration;
    struct server_xdg_wm_base_g *xdg_shell;

    struct {
        struct wl_signal input_focus;    // data: struct server_view *
        struct wl_signal pointer_lock;   // data: NULL
        struct wl_signal pointer_unlock; // data: NULL
    } events;
};

struct server_seat_listener {
    bool (*button)(void *data, uint32_t button, bool state);
    bool (*key)(void *data, uint32_t key, bool state);
    void (*modifiers)(void *data, uint32_t depressed, uint32_t latched, uint32_t locked,
                      uint32_t group);
    void (*motion)(void *data, double x, double y);

    void (*keymap)(void *data, int fd, uint32_t size);
};

struct syn_key {
    uint32_t keycode;
    bool press;
};

struct server *server_create(struct config *cfg);
void server_destroy(struct server *server);

struct wl_keyboard *server_get_wl_keyboard(struct server *server);
struct wl_pointer *server_get_wl_pointer(struct server *server);
void server_set_pointer_pos(struct server *server, double x, double y);
void server_set_seat_listener(struct server *server, const struct server_seat_listener *listener,
                              void *data);
void server_set_input_focus(struct server *server, struct server_view *view);
void server_shutdown(struct server *server);

bool server_view_has_focus(struct server_view *view);
void server_view_send_click(struct server_view *view);
void server_view_send_keys(struct server_view *view, const struct syn_key *keys, size_t num_keys);

#endif
