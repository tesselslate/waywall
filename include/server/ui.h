#ifndef WAYWALL_SERVER_UI_H
#define WAYWALL_SERVER_UI_H

#include <stdbool.h>
#include <stdint.h>

struct server_ui {
    struct server *server;

    struct wl_buffer *background;
    struct wl_surface *surface;
    struct wp_viewport *viewport;

    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;

    int32_t width, height;
    bool mapped;
};

void server_ui_destroy(struct server_ui *ui);
int server_ui_init(struct server *server, struct server_ui *ui);
void server_ui_hide(struct server_ui *ui);
void server_ui_show(struct server_ui *ui);

#endif
