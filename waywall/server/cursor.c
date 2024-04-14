#include "server/cursor.h"
#include "config/config.h"
#include "server/backend.h"
#include "server/server.h"
#include "server/wl_seat.h"
#include "util/alloc.h"
#include "util/log.h"
#include <wayland-client.h>
#include <wayland-cursor.h>

static void
on_pointer_enter(struct wl_listener *listener, void *data) {
    struct server_cursor *cursor = wl_container_of(listener, cursor, on_pointer_enter);
    uint32_t *serial = data;
    cursor->last_enter = *serial;

    wl_pointer_set_cursor(server_get_wl_pointer(cursor->server), cursor->last_enter,
                          cursor->show ? cursor->surface : NULL,
                          cursor->show ? cursor->config->image->hotspot_x : 0,
                          cursor->show ? cursor->config->image->hotspot_y : 0);
}

struct server_cursor *
server_cursor_create(struct server *server, struct config *cfg) {
    struct server_cursor *cursor = zalloc(1, sizeof(*cursor));

    cursor->server = server;

    cursor->surface = wl_compositor_create_surface(server->backend->compositor);
    check_alloc(cursor->surface);

    cursor->on_pointer_enter.notify = on_pointer_enter;
    wl_signal_add(&server->seat->events.pointer_enter, &cursor->on_pointer_enter);

    struct server_cursor_config *config = server_cursor_config_create(cursor, cfg);
    if (!config) {
        ww_log(LOG_ERROR, "failed to create server cursor config");
        goto fail_config;
    }
    server_cursor_use_config(cursor, config);

    return cursor;

fail_config:
    wl_surface_destroy(cursor->surface);
    wl_list_remove(&cursor->on_pointer_enter.link);
    free(cursor);
    return NULL;
}

void
server_cursor_destroy(struct server_cursor *cursor) {
    server_cursor_config_destroy(cursor->config);
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
    wl_pointer_set_cursor(server_get_wl_pointer(cursor->server), cursor->last_enter, NULL, 0, 0);
}

void
server_cursor_show(struct server_cursor *cursor) {
    if (cursor->show) {
        return;
    }

    cursor->show = true;
    wl_pointer_set_cursor(server_get_wl_pointer(cursor->server), cursor->last_enter,
                          cursor->surface, cursor->config->image->hotspot_x,
                          cursor->config->image->hotspot_y);
}

void
server_cursor_use_config(struct server_cursor *cursor, struct server_cursor_config *config) {
    if (cursor->config) {
        server_cursor_config_destroy(cursor->config);
    }
    cursor->config = config;

    wl_surface_attach(cursor->surface, cursor->config->buffer, 0, 0);
    wl_surface_commit(cursor->surface);

    if (cursor->show) {
        wl_pointer_set_cursor(server_get_wl_pointer(cursor->server), cursor->last_enter,
                              cursor->surface, cursor->config->image->hotspot_x,
                              cursor->config->image->hotspot_y);
    }
}

struct server_cursor_config *
server_cursor_config_create(struct server_cursor *cursor, struct config *cfg) {
    struct server_cursor_config *config = zalloc(1, sizeof(*config));

    config->theme = wl_cursor_theme_load(cfg->theme.cursor_theme, cfg->theme.cursor_size,
                                         cursor->server->backend->shm);
    if (!config->theme) {
        ww_log(LOG_ERROR, "failed to load cursor theme '%s'", cfg->theme.cursor_theme);
        goto fail_theme;
    }

    struct wl_cursor *wl_cursor = wl_cursor_theme_get_cursor(config->theme, cfg->theme.cursor_icon);
    if (!wl_cursor) {
        ww_log(LOG_ERROR, "cursor theme '%s' does not contain '%s' cursor icon",
               cfg->theme.cursor_theme, cfg->theme.cursor_icon);
        goto fail_get_cursor;
    }

    // As of libwayland commit aa2a6d5, this should always be true.
    ww_assert(wl_cursor->image_count > 0);

    config->image = wl_cursor->images[0];
    config->buffer = wl_cursor_image_get_buffer(config->image);
    if (!config->buffer) {
        ww_log(LOG_ERROR, "failed to get cursor image buffer");
        goto fail_buffer;
    }

    return config;

fail_buffer:
fail_get_cursor:
    wl_cursor_theme_destroy(config->theme);

fail_theme:
    free(config);
    return NULL;
}

void
server_cursor_config_destroy(struct server_cursor_config *config) {
    wl_cursor_theme_destroy(config->theme);
    free(config);
}
