#include "server/server.h"
#include "util.h"
#include <signal.h>
#include <wayland-server-core.h>

static int
handle_signal(int signal, void *data) {
    struct server *server = data;
    server_shutdown(server);
    return 0;
}

static void
on_view_create(struct wl_listener *listener, void *data) {
    struct server_view *view = data;
    ww_log(LOG_INFO, "create %p", view);
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
    struct server *server = server_create();
    if (!server) {
        return 1;
    }

    struct wl_listener view_create, view_destroy;
    view_create.notify = on_view_create;
    wl_signal_add(&server->ui.events.view_create, &view_create);

    view_destroy.notify = on_view_destroy;
    wl_signal_add(&server->ui.events.view_destroy, &view_destroy);

    server_set_seat_listener(server, &seat_listener, NULL);

    struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
    struct wl_event_source *src_sigint =
        wl_event_loop_add_signal(loop, SIGINT, handle_signal, server);

    const char *socket_name = wl_display_add_socket_auto(server->display);
    if (!socket_name) {
        ww_log(LOG_ERROR, "failed to create wayland display socket");
        goto fail_socket;
    }
    wl_display_run(server->display);

    wl_event_source_remove(src_sigint);
    server_destroy(server);

    ww_log(LOG_INFO, "Done");
    return 0;

fail_socket:
    wl_event_source_remove(src_sigint);
    server_destroy(server);
    return 1;
}
