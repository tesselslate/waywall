#include "server/server.h"
#include "config/config.h"
#include "server/backend.h"
#include "server/cursor.h"
#include "server/remote_buffer.h"
#include "server/ui.h"
#include "server/wl_compositor.h"
#include "server/wl_seat.h"
#include "server/wl_shm.h"
#include "server/wp_linux_dmabuf.h"
#include "server/wp_pointer_constraints.h"
#include "server/wp_relative_pointer.h"
#include "server/xdg_decoration.h"
#include "server/xdg_shell.h"
#include "util.h"
#include <string.h>
#include <wayland-client.h>
#include <wayland-cursor.h>

static int
backend_display_tick(int fd, uint32_t mask, void *data) {
    struct server *server = data;

    // Adapted from wlroots @ 31c842e5ece93145604c65be1b14c2f8cee24832
    // backend/wayland/backend.c:54

    if (mask & WL_EVENT_HANGUP) {
        ww_log(LOG_ERROR, "remote display hung up");
        wl_display_terminate(server->display);
        return 0;
    }

    if (mask & WL_EVENT_ERROR) {
        ww_log(LOG_ERROR, "failed to read events from remote display");
        wl_display_terminate(server->display);
        return 0;
    }

    if (mask & WL_EVENT_WRITABLE) {
        wl_display_flush(server->backend->display);
    }

    int dispatched = 0;
    if (mask & WL_EVENT_READABLE) {
        dispatched = wl_display_dispatch(server->backend->display);
    } else {
        dispatched = wl_display_dispatch_pending(server->backend->display);
        wl_display_flush(server->backend->display);
    }

    if (dispatched < 0) {
        ww_log(LOG_ERROR, "failed to dispatch events on remote display");
        wl_display_terminate(server->display);
        return 0;
    }

    return dispatched > 0;
}

static void
on_view_destroy(struct wl_listener *listener, void *data) {
    struct server *server = wl_container_of(listener, server, on_view_destroy);

    server->input_focus = NULL;
    wl_signal_emit_mutable(&server->events.input_focus, NULL);

    wl_list_remove(&server->on_view_destroy.link);
}

struct server *
server_create(struct config *cfg) {
    struct server *server = calloc(1, sizeof(*server));
    if (!server) {
        ww_log(LOG_ERROR, "failed to allocate server");
        return NULL;
    }

    server->cfg = cfg;

    wl_signal_init(&server->events.input_focus);
    wl_signal_init(&server->events.pointer_lock);   // used by pointer constraints
    wl_signal_init(&server->events.pointer_unlock); // used by pointer constraints

    server->on_view_destroy.notify = on_view_destroy;

    server->backend = server_backend_create();
    if (!server->backend) {
        goto fail_backend;
    }

    server->display = wl_display_create();
    if (!server->display) {
        goto fail_display;
    }

    struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
    server->backend_source = wl_event_loop_add_fd(loop, wl_display_get_fd(server->backend->display),
                                                  WL_EVENT_READABLE, backend_display_tick, server);
    ww_assert(server->backend_source);
    wl_event_source_check(server->backend_source);

    server->remote_buf = remote_buffer_manager_create(server);
    if (!server->remote_buf) {
        goto fail_remote_buf;
    }

    // These globals are required by others.
    server->compositor = server_compositor_create(server);
    if (!server->compositor) {
        goto fail_globals;
    }
    server->seat = server_seat_create(server, server->cfg);
    if (!server->seat) {
        goto fail_globals;
    }

    server->linux_dmabuf = server_linux_dmabuf_create(server);
    if (!server->linux_dmabuf) {
        goto fail_globals;
    }
    server->pointer_constraints = server_pointer_constraints_create(server);
    if (!server->pointer_constraints) {
        goto fail_globals;
    }
    server->relative_pointer = server_relative_pointer_create(server, server->cfg);
    if (!server->relative_pointer) {
        goto fail_globals;
    }
    server->shm = server_shm_create(server);
    if (!server->shm) {
        goto fail_globals;
    }
    server->xdg_decoration = server_xdg_decoration_manager_create(server);
    if (!server->xdg_decoration) {
        goto fail_globals;
    }
    server->xdg_shell = server_xdg_wm_base_create(server);
    if (!server->xdg_shell) {
        goto fail_globals;
    }

    server->cursor = server_cursor_create(server);
    if (!server->cursor) {
        ww_log(LOG_ERROR, "failed to initialize cursor");
        goto fail_cursor;
    }
    if (server_cursor_use_theme(server->cursor, server->cfg->theme.cursor_theme,
                                server->cfg->theme.cursor_size) != 0) {
        ww_log(LOG_ERROR, "failed to initialize cursor theme '%s'",
               server->cfg->theme.cursor_theme);
        goto fail_cursor_theme;
    }
    server_cursor_show(server->cursor);

    server->ui = server_ui_create(server, cfg);
    if (!server->ui) {
        ww_log(LOG_ERROR, "failed to initialize server_ui");
        goto fail_ui;
    }
    server_ui_show(server->ui);

    return server;

fail_ui:
fail_cursor_theme:
    server_cursor_destroy(server->cursor);

fail_cursor:
fail_globals:
    remote_buffer_manager_destroy(server->remote_buf);

fail_remote_buf:
    wl_event_source_remove(server->backend_source);
    wl_display_destroy(server->display);

fail_display:
    server_backend_destroy(server->backend);

fail_backend:
    free(server);
    return NULL;
}

void
server_destroy(struct server *server) {
    wl_event_source_remove(server->backend_source);

    wl_display_destroy_clients(server->display);
    wl_display_destroy(server->display);

    server_ui_destroy(server->ui);
    remote_buffer_manager_destroy(server->remote_buf);

    server_cursor_destroy(server->cursor);

    server_backend_destroy(server->backend);
    free(server);
}

struct wl_keyboard *
server_get_wl_keyboard(struct server *server) {
    if (server->backend->seat.keyboard) {
        return server->backend->seat.keyboard;
    }

    if (!server->backend->seat.remote) {
        return NULL;
    }

    if (!(server->backend->seat.caps & WL_SEAT_CAPABILITY_KEYBOARD)) {
        return NULL;
    }

    server->backend->seat.keyboard = wl_seat_get_keyboard(server->backend->seat.remote);
    ww_assert(server->backend->seat.keyboard);
    return server->backend->seat.keyboard;
}

struct wl_pointer *
server_get_wl_pointer(struct server *server) {
    if (server->backend->seat.pointer) {
        return server->backend->seat.pointer;
    }

    if (!server->backend->seat.remote) {
        return NULL;
    }

    if (!(server->backend->seat.caps & WL_SEAT_CAPABILITY_POINTER)) {
        return NULL;
    }

    server->backend->seat.pointer = wl_seat_get_pointer(server->backend->seat.remote);
    ww_assert(server->backend->seat.pointer);
    return server->backend->seat.pointer;
}

void
server_set_pointer_pos(struct server *server, double x, double y) {
    server_pointer_constraints_set_hint(server->pointer_constraints, x, y);
    wl_surface_commit(server->ui->surface);
}

void
server_set_seat_listener(struct server *server, const struct server_seat_listener *listener,
                         void *data) {
    server_seat_set_listener(server->seat, listener, data);
}

void
server_set_input_focus(struct server *server, struct server_view *view) {
    if (server->input_focus == view) {
        return;
    }

    if (server->input_focus) {
        wl_list_remove(&server->on_view_destroy.link);
    }

    server->input_focus = view;
    wl_signal_emit_mutable(&server->events.input_focus, server->input_focus);

    if (server->input_focus) {
        wl_signal_add(&view->events.destroy, &server->on_view_destroy);
    }
}

void
server_shutdown(struct server *server) {
    // TODO: make graceful (request window close, timeout)
    wl_display_terminate(server->display);
}

bool
server_view_has_focus(struct server_view *view) {
    return view->ui->server->input_focus == view;
}
