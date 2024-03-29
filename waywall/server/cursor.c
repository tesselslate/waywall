#include "server/cursor.h"
#include "config/config.h"
#include "server/backend.h"
#include "server/server.h"
#include "server/wl_seat.h"
#include "util.h"
#include <wayland-client.h>
#include <wayland-cursor.h>

static void
on_pointer_enter(struct wl_listener *listener, void *data) {
    struct server_cursor *cursor = wl_container_of(listener, cursor, on_pointer_enter);
    uint32_t *serial = data;
    cursor->last_enter = *serial;

    if (cursor->image) {
        wl_pointer_set_cursor(server_get_wl_pointer(cursor->server), cursor->last_enter,
                              cursor->surface, cursor->image->hotspot_x, cursor->image->hotspot_y);
    }
}

struct server_cursor *
server_cursor_create(struct server *server) {
    struct server_cursor *cursor = zalloc(1, sizeof(*cursor));

    cursor->server = server;

    cursor->surface = wl_compositor_create_surface(server->backend->compositor);
    check_alloc(cursor->surface);

    cursor->on_pointer_enter.notify = on_pointer_enter;
    wl_signal_add(&server->seat->events.pointer_enter, &cursor->on_pointer_enter);

    return cursor;
}

void
server_cursor_destroy(struct server_cursor *cursor) {
    if (cursor->theme) {
        wl_cursor_theme_destroy(cursor->theme);
    }

    wl_surface_destroy(cursor->surface);
    wl_list_remove(&cursor->on_pointer_enter.link);

    free(cursor);
}

void
server_cursor_hide(struct server_cursor *cursor) {
    if (!cursor->show) {
        return;
    }

    cursor->show = false;
    wl_surface_attach(cursor->surface, NULL, 0, 0);
    wl_surface_commit(cursor->surface);
}

void
server_cursor_show(struct server_cursor *cursor) {
    if (cursor->show) {
        return;
    }

    cursor->show = true;
    wl_surface_attach(cursor->surface, cursor->buffer, 0, 0);
    wl_surface_commit(cursor->surface);
}

int
server_cursor_use_theme(struct server_cursor *cursor, const char *name, const char *icon,
                        int size) {
    struct wl_cursor_theme *theme = wl_cursor_theme_load(name, size, cursor->server->backend->shm);
    if (!theme) {
        ww_log(LOG_ERROR, "failed to load cursor theme '%s'", name);
        return 1;
    }

    struct wl_cursor *wl_cursor = wl_cursor_theme_get_cursor(theme, icon);
    if (!wl_cursor) {
        ww_log(LOG_ERROR, "cursor theme '%s' does not contain '%s' cursor icon", name, icon);
        goto fail_get_cursor;
    }

    // As of libwayland commit aa2a6d5, this should always be true.
    ww_assert(wl_cursor->image_count > 0);

    struct wl_cursor_image *image = wl_cursor->images[0];
    struct wl_buffer *buffer = wl_cursor_image_get_buffer(image);
    if (!buffer) {
        ww_log(LOG_ERROR, "failed to get cursor image buffer");
        goto fail_buffer;
    }

    // Commit the changes.
    if (cursor->theme) {
        wl_cursor_theme_destroy(cursor->theme);
        cursor->theme = NULL;
    }
    if (cursor->buffer) {
        cursor->buffer = NULL;
    }

    cursor->theme = theme;
    cursor->image = image;
    cursor->buffer = buffer;

    if (cursor->show) {
        wl_pointer_set_cursor(server_get_wl_pointer(cursor->server), cursor->last_enter,
                              cursor->surface, cursor->image->hotspot_x, cursor->image->hotspot_y);
        wl_surface_attach(cursor->surface, cursor->buffer, 0, 0);
        wl_surface_commit(cursor->surface);
    }

    return 0;

fail_buffer:
fail_get_cursor:
    wl_cursor_theme_destroy(theme);
    return 1;
}
