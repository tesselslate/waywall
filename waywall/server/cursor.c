#include "server/cursor.h"
#include "config/config.h"
#include "cursor-shape-v1-client-protocol.h"
#include "server/backend.h"
#include "server/server.h"
#include "server/wl_seat.h"
#include "util/alloc.h"
#include "util/log.h"
#include <wayland-client.h>
#include <wayland-cursor.h>

#define DEFAULT_ICON "left_ptr"
#define DEFAULT_SIZE 16
#define DEFAULT_THEME "default"

// These are based off what I see in the freedesktop.org draft spec (it's from, uh, 2003...), as
// well as in the cursor theme I personally use (BreezeX-Dark). If there are better resources to
// reference, feel free to let me know.
//
// See: https://www.freedesktop.org/wiki/Specifications/cursor-spec/
static const struct {
    const char *name;
    enum wp_cursor_shape_device_v1_shape shape;
} shape_mappings[] = {
    // Present
    {"default", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT},
    {"context-menu", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CONTEXT_MENU},
    {"help", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP},
    {"pointer", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER},
    {"progress", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS},
    {"wait", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT},
    {"cell", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CELL},
    {"crosshair", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR},
    {"text", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT},
    {"vertical-text", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_VERTICAL_TEXT},
    {"alias", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS},
    {"copy", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY},
    {"no-drop", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP},
    {"not-allowed", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED},
    {"e-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE},
    {"n-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE},
    {"ne-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE},
    {"nw-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE},
    {"s-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE},
    {"se-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE},
    {"sw-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE},
    {"w-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE},
    {"col-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE},
    {"row-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE},
    {"all-scroll", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL},

    // "Up for discussion"
    {"ew-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE},
    {"ns-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE},
    {"nesw-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE},
    {"nwse-resize", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE},

    // Not present
    {"left_ptr", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT},
    {"move", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE},
    {"grab", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB},
    {"grabbing", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING},
    {"zoom-in", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN},
    {"zoom-out", WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT},
};

struct xcursor_options {
    const char *theme;
    const char *icon;
    int size;
};

static int
create_config_xcursor(struct server_cursor *cursor, struct server_cursor_config *config,
                      struct xcursor_options opts) {
    config->type = SERVER_CURSOR_CONFIG_TYPE_XCURSOR;

    config->data.xcursor.theme =
        wl_cursor_theme_load(opts.theme, opts.size, cursor->server->backend->shm);
    if (!config->data.xcursor.theme) {
        ww_log(LOG_ERROR, "failed to load cursor theme '%s'", opts.theme);
        return 1;
    }

    struct wl_cursor *wl_cursor = wl_cursor_theme_get_cursor(config->data.xcursor.theme, opts.icon);
    if (!wl_cursor) {
        ww_log(LOG_ERROR, "cursor theme '%s' does not contain '%s' cursor icon", opts.theme,
               opts.icon);
        goto fail_get_cursor;
    }

    // As of libwayland commit aa2a6d5, this should always be true.
    ww_assert(wl_cursor->image_count > 0);

    config->data.xcursor.image = wl_cursor->images[0];
    config->data.xcursor.buffer = wl_cursor_image_get_buffer(config->data.xcursor.image);
    if (!config->data.xcursor.buffer) {
        ww_log(LOG_ERROR, "failed to get cursor image buffer");
        goto fail_buffer;
    }

    return 0;

fail_buffer:
fail_get_cursor:
    wl_cursor_theme_destroy(config->data.xcursor.theme);
    return 1;
}

static void
hide_cursor(struct server_cursor *cursor) {
    wl_pointer_set_cursor(server_get_wl_pointer(cursor->server), cursor->last_enter, NULL, 0, 0);
}

static void
show_cursor(struct server_cursor *cursor) {
    switch (cursor->config->type) {
    case SERVER_CURSOR_CONFIG_TYPE_XCURSOR:
        wl_pointer_set_cursor(server_get_wl_pointer(cursor->server), cursor->last_enter,
                              cursor->surface, cursor->config->data.xcursor.image->hotspot_x,
                              cursor->config->data.xcursor.image->hotspot_y);
        break;
    case SERVER_CURSOR_CONFIG_TYPE_SHAPE:
        ww_assert(cursor->shape_device);
        wp_cursor_shape_device_v1_set_shape(cursor->shape_device, cursor->last_enter,
                                            cursor->config->data.shape);
        break;
    }
}

static void
on_pointer(struct wl_listener *listener, void *data) {
    struct server_cursor *cursor = wl_container_of(listener, cursor, on_pointer);

    if (cursor->shape_device) {
        wp_cursor_shape_device_v1_destroy(cursor->shape_device);
        cursor->shape_device = wp_cursor_shape_manager_v1_get_pointer(
            cursor->server->backend->cursor_shape_manager, server_get_wl_pointer(cursor->server));
        check_alloc(cursor->shape_device);
    }
}

static void
on_pointer_enter(struct wl_listener *listener, void *data) {
    struct server_cursor *cursor = wl_container_of(listener, cursor, on_pointer_enter);
    uint32_t *serial = data;
    cursor->last_enter = *serial;

    if (cursor->show) {
        show_cursor(cursor);
    } else {
        hide_cursor(cursor);
    }
}

struct server_cursor *
server_cursor_create(struct server *server, struct config *cfg) {
    struct server_cursor *cursor = zalloc(1, sizeof(*cursor));

    cursor->server = server;

    cursor->surface = wl_compositor_create_surface(server->backend->compositor);
    check_alloc(cursor->surface);

    if (server->backend->cursor_shape_manager) {
        cursor->shape_device = wp_cursor_shape_manager_v1_get_pointer(
            server->backend->cursor_shape_manager, server_get_wl_pointer(server));
        check_alloc(cursor->shape_device);
    }

    cursor->on_pointer.notify = on_pointer;
    wl_signal_add(&server->backend->events.seat_pointer, &cursor->on_pointer);

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
    if (cursor->shape_device) {
        wp_cursor_shape_device_v1_destroy(cursor->shape_device);
    }
    wl_list_remove(&cursor->on_pointer.link);
    wl_list_remove(&cursor->on_pointer_enter.link);

    free(cursor);
}

void
server_cursor_hide(struct server_cursor *cursor) {
    if (!cursor->show) {
        return;
    }

    cursor->show = false;
    hide_cursor(cursor);
}

void
server_cursor_show(struct server_cursor *cursor) {
    if (cursor->show) {
        return;
    }

    cursor->show = true;
    show_cursor(cursor);
}

void
server_cursor_use_config(struct server_cursor *cursor, struct server_cursor_config *config) {
    if (cursor->config) {
        server_cursor_config_destroy(cursor->config);
    }
    cursor->config = config;

    if (cursor->config->type == SERVER_CURSOR_CONFIG_TYPE_XCURSOR) {
        wl_surface_attach(cursor->surface, cursor->config->data.xcursor.buffer, 0, 0);
        wl_surface_commit(cursor->surface);
    }

    if (cursor->show) {
        show_cursor(cursor);
    }
}

struct server_cursor_config *
server_cursor_config_create(struct server_cursor *cursor, struct config *cfg) {
    struct server_cursor_config *config = zalloc(1, sizeof(*config));

    // The user may set zero or more of the three cursor options (theme, image, and size.)
    //
    // Rules for resolving cursor size:
    // - If the `cursor_size` option was set, use that.
    // - Use the `XCURSOR_SIZE` environment variable if it is present.
    // - Default to a size of 16.
    //
    // Rules for resolving cursor theme:
    // - If the `cursor_theme` option was set, use that.
    // - Use the `XCURSOR_THEME` environment variable if it is present.
    // - Default to `default` (or use cursor-shape if possible.)
    //
    // Cursor image will default to `left_ptr` unless otherwise specified.
    // If no theme or size was provided, attempt to use the cursor-shape protocol if possible.

    struct xcursor_options opts = {
        .theme = NULL,
        .icon = NULL,
        .size = -1,
    };

    bool has_theme = (strcmp(cfg->theme.cursor_theme, "") != 0);
    bool has_icon = (strcmp(cfg->theme.cursor_icon, "") != 0);
    bool has_size = cfg->theme.cursor_size > 0;

    if (has_size) {
        opts.size = cfg->theme.cursor_size;
    } else {
        const char *xcursor_size = getenv("XCURSOR_SIZE");
        if (xcursor_size) {
            char *endptr;
            long size = strtol(xcursor_size, &endptr, 10);
            if (!*endptr && size > 0) {
                opts.size = size;
            }
        }
    }

    if (has_theme) {
        opts.theme = cfg->theme.cursor_theme;
    } else {
        const char *xcursor_theme = getenv("XCURSOR_THEME");
        if (xcursor_theme) {
            opts.theme = xcursor_theme;
        }
    }

    if (has_icon) {
        opts.icon = cfg->theme.cursor_icon;
    }

    // If a theme was provided, load that and return.
    if (has_theme) {
        if (!has_icon) {
            opts.icon = DEFAULT_ICON;
        }
        if (opts.size == -1) {
            opts.size = DEFAULT_SIZE;
        }

        if (create_config_xcursor(cursor, config, opts) != 0) {
            goto fail_theme;
        }

        return config;
    }

    // If it is not possible to use the cursor-shape protocol, attempt to load the theme manually.
    if (has_size || !cursor->server->backend->cursor_shape_manager) {
        if (!has_icon) {
            opts.icon = DEFAULT_ICON;
        }
        if (opts.size == -1) {
            opts.size = DEFAULT_SIZE;
        }
        if (!opts.theme) {
            opts.theme = DEFAULT_THEME;
        }

        if (create_config_xcursor(cursor, config, opts) != 0) {
            goto fail_theme;
        }

        return config;
    }

    // Otherwise, use the cursor-shape protocol.
    config->type = SERVER_CURSOR_CONFIG_TYPE_SHAPE;
    config->data.shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;

    if (has_icon) {
        for (size_t i = 0; i < STATIC_ARRLEN(shape_mappings); i++) {
            if (strcmp(shape_mappings[i].name, opts.icon) == 0) {
                config->data.shape = shape_mappings[i].shape;
                break;
            }
        }
    }

    return config;

fail_theme:
    free(config);
    return NULL;
}

void
server_cursor_config_destroy(struct server_cursor_config *config) {
    switch (config->type) {
    case SERVER_CURSOR_CONFIG_TYPE_XCURSOR:
        wl_cursor_theme_destroy(config->data.xcursor.theme);
        break;
    case SERVER_CURSOR_CONFIG_TYPE_SHAPE:
        break;
    }
    free(config);
}
