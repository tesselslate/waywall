#ifndef WAYWALL_SERVER_WL_SEAT_H
#define WAYWALL_SERVER_WL_SEAT_H

#include <wayland-server-core.h>

struct server;
struct server_surface;
struct syn_key;

struct server_seat_g {
    struct wl_global *global;

    struct config *cfg;
    struct server *server;

    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct {
        int32_t local_keymap_fd;
        uint32_t local_keymap_size;

        int32_t remote_keymap_fd;
        uint32_t remote_keymap_size;
        struct xkb_keymap *remote_keymap;
        int32_t repeat_rate, repeat_delay;

        uint32_t mods_depressed, mods_latched, mods_locked, group;
        struct {
            uint32_t *data;
            size_t cap, len;
        } pressed;
    } kb_state;
    struct {
        double x, y;
    } ptr_state;

    struct server_view *input_focus;
    struct wl_listener on_input_focus;

    const struct server_seat_listener *listener;
    void *listener_data;

    struct wl_list keyboards; // wl_resource link
    struct wl_list pointers;  // wl_resource link

    struct wl_listener on_keyboard;
    struct wl_listener on_pointer;

    struct wl_listener on_display_destroy;

    struct {
        struct wl_signal pointer_enter; // data: uint32_t *
    } events;
};

struct server_seat_g *server_seat_g_create(struct server *server, struct config *cfg);
void server_seat_g_send_click(struct server_seat_g *seat_g, struct server_view *view);
void server_seat_g_send_keys(struct server_seat_g *seat_g, struct server_view *view,
                             const struct syn_key *keys, size_t num_keys);
void server_seat_g_set_listener(struct server_seat_g *seat_g,
                                const struct server_seat_listener *listener, void *data);

#endif
