#ifndef WAYWALL_SERVER_FAKE_INPUT_H
#define WAYWALL_SERVER_FAKE_INPUT_H

#include "server/server.h"
#include "server/ui.h"
#include "server/wl_seat.h"

static inline void
server_view_send_click(struct server_view *view) {
    struct server_seat *seat = view->ui->server->seat;

    server_seat_send_click(seat, view);
}

static inline void
server_view_send_keys(struct server_view *view, size_t num_keys,
                      const struct syn_key keys[static num_keys]) {
    struct server_seat *seat = view->ui->server->seat;

    server_seat_send_keys(seat, view, num_keys, keys);
}

#endif
