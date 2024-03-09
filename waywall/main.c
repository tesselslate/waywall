#include "server/server.h"
#include "util.h"
#include <signal.h>
#include <wayland-server-core.h>

struct ww_state {
    struct server *server;

    struct wl_listener on_view_create;
    struct wl_listener on_view_destroy;
};

static int
handle_signal(int signal, void *data) {
    struct server *server = data;
    server_shutdown(server);
    return 0;
}

static void
on_view_create(struct wl_listener *listener, void *data) {
    struct ww_state *ww = wl_container_of(listener, ww, on_view_create);
    struct server_view *view = data;
    ww_log(LOG_INFO, "create %p", view);

    server_set_input_focus(ww->server, view);
    server_view_show(view);
}

static void
on_view_destroy(struct wl_listener *listener, void *data) {
    struct server_view *view = data;
    ww_log(LOG_INFO, "destroy %p", view);
}

static bool
on_button(void *data, uint32_t button, bool pressed) {
    ww_log(LOG_INFO, "button %d\t%d", button, pressed);
    return false;
}

static bool
on_key(void *data, uint32_t key, bool pressed) {
    ww_log(LOG_INFO, "key %d\t%d", key, pressed);
    return false;
}

static void
on_motion(void *data, double x, double y) {
    ww_log(LOG_INFO, "motion %lf\t%lf", x, y);
}

static void
on_keymap(void *data, int fd, uint32_t size) {
    ww_log(LOG_INFO, "keymap %d\t%u", fd, size);
}

static const struct server_seat_listener seat_listener = {
    .button = on_button,
    .key = on_key,
    .motion = on_motion,

    .keymap = on_keymap,
};

int
main() {
    struct ww_state ww = {0};
    ww.server = server_create();
    if (!ww.server) {
        return 1;
    }

    ww.on_view_create.notify = on_view_create;
    wl_signal_add(&ww.server->ui.events.view_create, &ww.on_view_create);

    ww.on_view_destroy.notify = on_view_destroy;
    wl_signal_add(&ww.server->ui.events.view_destroy, &ww.on_view_destroy);

    server_set_seat_listener(ww.server, &seat_listener, NULL);

    struct wl_event_loop *loop = wl_display_get_event_loop(ww.server->display);
    struct wl_event_source *src_sigint =
        wl_event_loop_add_signal(loop, SIGINT, handle_signal, ww.server);

    const char *socket_name = wl_display_add_socket_auto(ww.server->display);
    if (!socket_name) {
        ww_log(LOG_ERROR, "failed to create wayland display socket");
        goto fail_socket;
    }
    wl_display_run(ww.server->display);

    wl_event_source_remove(src_sigint);
    server_destroy(ww.server);

    ww_log(LOG_INFO, "Done");
    return 0;

fail_socket:
    wl_event_source_remove(src_sigint);
    server_destroy(ww.server);
    return 1;
}
