#include "inotify.h"
#include "server/server.h"
#include "util.h"
#include "wall.h"
#include <signal.h>
#include <wayland-server-core.h>

static int
handle_signal(int signal, void *data) {
    struct server *server = data;
    server_shutdown(server);
    return 0;
}

int
main() {
    struct server *server = server_create();
    if (!server) {
        return 1;
    }

    struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
    struct wl_event_source *src_sigint =
        wl_event_loop_add_signal(loop, SIGINT, handle_signal, server);

    struct inotify *inotify = inotify_create(loop);
    if (!inotify) {
        goto fail_inotify;
    }

    struct wall *wall = wall_create(server, inotify);
    if (!wall) {
        goto fail_wall;
    }

    const char *socket_name = wl_display_add_socket_auto(server->display);
    if (!socket_name) {
        ww_log(LOG_ERROR, "failed to create wayland display socket");
        goto fail_socket;
    }
    wl_display_run(server->display);

    wall_destroy(wall);
    inotify_destroy(inotify);
    wl_event_source_remove(src_sigint);
    server_destroy(server);

    ww_log(LOG_INFO, "Done");
    return 0;

fail_wall:
    inotify_destroy(inotify);

fail_socket:
    wall_destroy(wall);

fail_inotify:
    wl_event_source_remove(src_sigint);
    server_destroy(server);
    return 1;
}
