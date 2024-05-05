#include "server/fake_input.h"
#include "server/server.h"
#include "server/ui.h"
#include "server/wl_seat.h"
#include "string.h"
#include "util/prelude.h"

#ifdef WAYWALL_XWAYLAND

#include "server/xwayland.h"

#endif

static inline void
wl_send_click(struct server_view *view) {
    server_seat_send_click(view->ui->server->seat, view);
}

static inline void
wl_send_keys(struct server_view *view, size_t num_keys,
             const struct syn_key keys[static num_keys]) {
    server_seat_send_keys(view->ui->server->seat, view, num_keys, keys);
}

#ifdef WAYWALL_XWAYLAND

static inline void
x11_send_click(struct server_view *view) {
    xwl_send_click(view->ui->server->xwayland, view);
}

static inline void
x11_send_keys(struct server_view *view, size_t num_keys,
              const struct syn_key keys[static num_keys]) {
    xwl_send_keys(view->ui->server->xwayland, view, num_keys, keys);
}

#else

static inline void
x11_send_click(struct server_view *view) {
    ww_unreachable();
}

static inline void
x11_send_keys(struct server_view *view, size_t num_keys,
              const struct syn_key keys[static num_keys]) {
    ww_unreachable();
}

#endif

void
server_view_send_click(struct server_view *view) {
    if (strcmp(view->impl->name, "xdg_toplevel") == 0) {
        wl_send_click(view);
    } else if (strcmp(view->impl->name, "xwayland") == 0) {
        x11_send_click(view);
    } else {
        ww_unreachable();
    }
}

void
server_view_send_keys(struct server_view *view, size_t num_keys,
                      const struct syn_key keys[static num_keys]) {
    if (strcmp(view->impl->name, "xdg_toplevel") == 0) {
        wl_send_keys(view, num_keys, keys);
    } else if (strcmp(view->impl->name, "xwayland") == 0) {
        x11_send_keys(view, num_keys, keys);
    } else {
        ww_unreachable();
    }
}
