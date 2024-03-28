#ifndef WAYWALL_SERVER_SERVER_H
#define WAYWALL_SERVER_SERVER_H

#include <wayland-client-core.h>
#include <wayland-server-core.h>

struct config;

struct server {
    struct wl_display *display;
    struct server_backend *backend;
    struct server_ui *ui;

    struct server_view *input_focus;
    struct wl_listener on_view_destroy;

    struct server_cursor *cursor;

    struct wl_event_source *backend_source;

    struct remote_buffer_manager *remote_buf;

    struct server_compositor *compositor;
    struct server_linux_dmabuf *linux_dmabuf;
    struct server_pointer_constraints *pointer_constraints;
    struct server_relative_pointer *relative_pointer;
    struct server_seat *seat;
    struct server_shm *shm;
    struct server_xdg_decoration_manager *xdg_decoration;
    struct server_xdg_wm_base *xdg_shell;

    struct {
        struct wl_signal input_focus;    // data: struct server_view *
        struct wl_signal pointer_lock;   // data: NULL
        struct wl_signal pointer_unlock; // data: NULL
    } events;
};

struct server *server_create(struct config *cfg);
void server_destroy(struct server *server);
int server_set_config(struct server *server, struct config *cfg);

struct wl_keyboard *server_get_wl_keyboard(struct server *server);
struct wl_pointer *server_get_wl_pointer(struct server *server);
void server_set_pointer_pos(struct server *server, double x, double y);
void server_set_input_focus(struct server *server, struct server_view *view);
void server_shutdown(struct server *server);

bool server_view_has_focus(struct server_view *view);

#endif
