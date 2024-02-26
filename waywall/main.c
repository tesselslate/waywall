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

int
main() {
    struct server *server = server_create();
    if (!server) {
        return 1;
    }

    struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
    struct wl_event_source *src_sigint =
        wl_event_loop_add_signal(loop, SIGINT, handle_signal, server);

    wl_display_run(server->display);

    wl_event_source_remove(src_sigint);
    server_destroy(server);

    ww_log(LOG_INFO, "Done");
    return 0;
}
