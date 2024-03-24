#include "config/config.h"
#include "cpu/cgroup_setup.h"
#include "inotify.h"
#include "server/server.h"
#include "util.h"
#include "wall.h"
#include <signal.h>
#include <string.h>
#include <wayland-server-core.h>

static int
handle_signal(int signal, void *data) {
    struct server *server = data;
    server_shutdown(server);
    return 0;
}

static int
cmd_waywall() {
    struct config *cfg = config_create();
    if (!cfg) {
        return 1;
    }
    if (config_load(cfg) != 0) {
        goto fail_config_populate;
    }

    struct server *server = server_create(cfg);
    if (!server) {
        goto fail_server;
    }

    struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
    struct wl_event_source *src_sigint =
        wl_event_loop_add_signal(loop, SIGINT, handle_signal, server);

    struct inotify *inotify = inotify_create(loop);
    if (!inotify) {
        goto fail_inotify;
    }

    struct wall *wall = wall_create(server, inotify, cfg);
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
    config_destroy(cfg);

    ww_log(LOG_INFO, "Done");
    return 0;

fail_socket:
    wall_destroy(wall);

fail_wall:
    inotify_destroy(inotify);

fail_inotify:
    wl_event_source_remove(src_sigint);
    server_destroy(server);

fail_server:
fail_config_populate:
    config_destroy(cfg);

    return 1;
}

int
main(int argc, char **argv) {
    util_log_init();

    if (argc > 1) {
        if (strcmp(argv[1], "cpu") == 0) {
            return cgroup_setup_root();
        }
    }

    return cmd_waywall();
}
