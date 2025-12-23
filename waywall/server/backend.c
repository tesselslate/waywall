#include "server/backend.h"
#include "alpha-modifier-v1-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "linux-drm-syncobj-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "tearing-control-v1-client-protocol.h"
#include "util/alloc.h"
#include "util/log.h"
#include "viewporter-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#define USE_ALPHA_MODIFIER_VERSION 1
#define USE_COMPOSITOR_VERSION 5
#define USE_CURSOR_SHAPE_VERSION 1
#define USE_DATA_DEVICE_MANAGER_VERSION 2
#define USE_LINUX_DMABUF_VERSION 4
#define USE_LINUX_DRM_SYNCOBJ_VERSION 1
#define USE_POINTER_CONSTRAINTS_VERSION 1
#define USE_RELATIVE_POINTER_MANAGER_VERSION 1
#define USE_SEAT_VERSION 5
#define USE_SHM_VERSION 1
#define USE_SINGLE_PIXEL_BUFFER_VERSION 1
#define USE_SUBCOMPOSITOR_VERSION 1
#define USE_TEARING_CONTROL_VERSION 1
#define USE_VIEWPORTER_VERSION 1
#define USE_XDG_DECORATION_VERSION 1
#define USE_XDG_WM_BASE_VERSION 1

struct seat_name {
    struct wl_list link; // server_backend.seat.names
    uint32_t name;
};

static void
on_seat_capabilities(void *data, struct wl_seat *wl, uint32_t caps) {
    struct server_backend *backend = data;

    backend->seat.caps = caps;

    bool had_kb = !!backend->seat.keyboard;
    bool has_kb = caps & WL_SEAT_CAPABILITY_KEYBOARD;
    if (had_kb != has_kb) {
        if (backend->seat.keyboard) {
            wl_keyboard_release(backend->seat.keyboard);
            backend->seat.keyboard = NULL;
        }

        ww_log(LOG_INFO, "seat updated (keyboard: %s)", has_kb ? "true" : "false");
        wl_signal_emit_mutable(&backend->events.seat_keyboard, NULL);
    }

    bool had_ptr = !!backend->seat.pointer;
    bool has_ptr = caps & WL_SEAT_CAPABILITY_POINTER;
    if (had_ptr != has_ptr) {
        if (backend->seat.pointer) {
            wl_pointer_release(backend->seat.pointer);
            backend->seat.pointer = NULL;
        }

        ww_log(LOG_INFO, "seat updated (pointer: %s)", has_ptr ? "true" : "false");
        wl_signal_emit_mutable(&backend->events.seat_pointer, NULL);
    }
}

static void
on_seat_name(void *data, struct wl_seat *wl, const char *name) {
    // Unused.
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = on_seat_capabilities,
    .name = on_seat_name,
};

static void
on_shm_format(void *data, struct wl_shm *wl, uint32_t format) {
    struct server_backend *backend = data;

    uint32_t *next = wl_array_add(&backend->shm_formats, sizeof(*next));
    check_alloc(next);
    *next = format;

    wl_signal_emit_mutable(&backend->events.shm_format, next);
}

static const struct wl_shm_listener shm_listener = {
    .format = on_shm_format,
};

static void
on_xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial) {
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = on_xdg_wm_base_ping,
};

static void
on_registry_global(void *data, struct wl_registry *wl, uint32_t name, const char *iface,
                   uint32_t version) {
    struct server_backend *backend = data;

    if (strcmp(iface, wp_alpha_modifier_v1_interface.name) == 0) {
        if (version < USE_ALPHA_MODIFIER_VERSION) {
            ww_log(LOG_WARN, "host compositor provides outdated wp_alpha_modifier_v1 (%d < %d)",
                   version, USE_ALPHA_MODIFIER_VERSION);
            return;
        }

        backend->alpha_modifier =
            wl_registry_bind(wl, name, &wp_alpha_modifier_v1_interface, USE_ALPHA_MODIFIER_VERSION);
        check_alloc(backend->alpha_modifier);
    } else if (strcmp(iface, wl_compositor_interface.name) == 0) {
        if (version < USE_COMPOSITOR_VERSION) {
            ww_log(LOG_ERROR, "host compositor provides outdated wl_compositor (%d < %d)", version,
                   USE_COMPOSITOR_VERSION);
            return;
        }

        backend->compositor =
            wl_registry_bind(wl, name, &wl_compositor_interface, USE_COMPOSITOR_VERSION);
        check_alloc(backend->compositor);
    } else if (strcmp(iface, wp_cursor_shape_manager_v1_interface.name) == 0) {
        if (version < USE_CURSOR_SHAPE_VERSION) {
            ww_log(LOG_WARN, "host compositor provides outdated wp_cursor_shape_manager (%d < %d)",
                   version, USE_CURSOR_SHAPE_VERSION);
            return;
        }

        backend->cursor_shape_manager = wl_registry_bind(
            wl, name, &wp_cursor_shape_manager_v1_interface, USE_CURSOR_SHAPE_VERSION);
        check_alloc(backend->cursor_shape_manager);
    } else if (strcmp(iface, wl_data_device_manager_interface.name) == 0) {
        if (version < USE_DATA_DEVICE_MANAGER_VERSION) {
            ww_log(LOG_ERROR, "host compostior provides outdated wl_data_device_manager (%d < %d)",
                   version, USE_DATA_DEVICE_MANAGER_VERSION);
            return;
        }

        backend->data_device_manager = wl_registry_bind(wl, name, &wl_data_device_manager_interface,
                                                        USE_DATA_DEVICE_MANAGER_VERSION);
        check_alloc(backend->data_device_manager);
    } else if (strcmp(iface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        if (version < USE_LINUX_DMABUF_VERSION) {
            ww_log(LOG_ERROR, "host compositor provides outdated zwp_linux_dmabuf (%d < %d)",
                   version, USE_LINUX_DMABUF_VERSION);
            return;
        }

        backend->linux_dmabuf =
            wl_registry_bind(wl, name, &zwp_linux_dmabuf_v1_interface, USE_LINUX_DMABUF_VERSION);
        check_alloc(backend->linux_dmabuf);
    } else if (strcmp(iface, wp_linux_drm_syncobj_manager_v1_interface.name) == 0) {
        if (version < USE_LINUX_DRM_SYNCOBJ_VERSION) {
            ww_log(LOG_WARN,
                   "host compositor provides outdated wp_linux_drm_syncobj_manager (%d < %d)",
                   version, USE_LINUX_DRM_SYNCOBJ_VERSION);
            return;
        }

        backend->linux_drm_syncobj_manager = wl_registry_bind(
            wl, name, &wp_linux_drm_syncobj_manager_v1_interface, USE_LINUX_DRM_SYNCOBJ_VERSION);
        check_alloc(backend->linux_drm_syncobj_manager);
    } else if (strcmp(iface, zwp_pointer_constraints_v1_interface.name) == 0) {
        if (version < USE_POINTER_CONSTRAINTS_VERSION) {
            ww_log(LOG_ERROR, "host compositor provides outdated zwp_pointer_constraints (%d < %d)",
                   version, USE_POINTER_CONSTRAINTS_VERSION);
            return;
        }

        backend->pointer_constraints = wl_registry_bind(
            wl, name, &zwp_pointer_constraints_v1_interface, USE_POINTER_CONSTRAINTS_VERSION);
        check_alloc(backend->pointer_constraints);
    } else if (strcmp(iface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
        if (version < USE_RELATIVE_POINTER_MANAGER_VERSION) {
            ww_log(LOG_ERROR,
                   "host compositor provides outdated zwp_relative_pointer_manager (%d < %d)",
                   version, USE_RELATIVE_POINTER_MANAGER_VERSION);
            return;
        }

        backend->relative_pointer_manager =
            wl_registry_bind(wl, name, &zwp_relative_pointer_manager_v1_interface,
                             USE_RELATIVE_POINTER_MANAGER_VERSION);
        check_alloc(backend->relative_pointer_manager);
    } else if (strcmp(iface, wl_seat_interface.name) == 0) {
        if (version < USE_SEAT_VERSION) {
            ww_log(LOG_ERROR, "host compositor provides outdated wl_seat (%d < %d)", version,
                   USE_SEAT_VERSION);
            return;
        }

        struct seat_name *seat_name = zalloc(1, sizeof(*seat_name));

        seat_name->name = name;

        if (wl_list_empty(&backend->seat.names)) {
            backend->seat.remote = wl_registry_bind(wl, name, &wl_seat_interface, USE_SEAT_VERSION);
            check_alloc(backend->seat.remote);

            wl_seat_add_listener(backend->seat.remote, &seat_listener, backend);
            wl_display_roundtrip(backend->display);
        } else {
            ww_log(LOG_INFO, "received duplicate wl_seat");
        }

        wl_list_insert(&backend->seat.names, &seat_name->link);
        wl_signal_emit_mutable(&backend->events.seat_data_device, NULL);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        if (version < USE_SHM_VERSION) {
            ww_log(LOG_ERROR, "host compositor provides outdated wl_shm (%d < %d)", version,
                   USE_SHM_VERSION);
            return;
        }

        backend->shm = wl_registry_bind(wl, name, &wl_shm_interface, USE_SHM_VERSION);
        check_alloc(backend->shm);

        wl_shm_add_listener(backend->shm, &shm_listener, backend);
        wl_display_roundtrip(backend->display);
    } else if (strcmp(iface, wp_single_pixel_buffer_manager_v1_interface.name) == 0) {
        if (version < USE_SINGLE_PIXEL_BUFFER_VERSION) {
            ww_log(LOG_INFO,
                   "host compositor provides outdated wp_single_pixel_buffer_manager (%d < %d)",
                   version, USE_SINGLE_PIXEL_BUFFER_VERSION);
            return;
        }

        backend->single_pixel_buffer_manager =
            wl_registry_bind(wl, name, &wp_single_pixel_buffer_manager_v1_interface,
                             USE_SINGLE_PIXEL_BUFFER_VERSION);
    } else if (strcmp(iface, wl_subcompositor_interface.name) == 0) {
        if (version < USE_SUBCOMPOSITOR_VERSION) {
            ww_log(LOG_ERROR, "host compositor provides outdated wl_subcompositor (%d < %d)",
                   version, USE_SUBCOMPOSITOR_VERSION);
            return;
        }

        backend->subcompositor =
            wl_registry_bind(wl, name, &wl_subcompositor_interface, USE_SUBCOMPOSITOR_VERSION);
    } else if (strcmp(iface, wp_tearing_control_manager_v1_interface.name) == 0) {
        if (version < USE_TEARING_CONTROL_VERSION) {
            ww_log(LOG_WARN,
                   "host compositor provides outdated wp_tearing_control_manager (%d < %d)",
                   version, USE_TEARING_CONTROL_VERSION);
            return;
        }

        backend->tearing_control = wl_registry_bind(
            wl, name, &wp_tearing_control_manager_v1_interface, USE_TEARING_CONTROL_VERSION);
        check_alloc(backend->tearing_control);
    } else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
        if (version < USE_VIEWPORTER_VERSION) {
            ww_log(LOG_ERROR, "host compositor provides outdated wp_viewporter (%d < %d)", version,
                   USE_VIEWPORTER_VERSION);
            return;
        }

        backend->viewporter =
            wl_registry_bind(wl, name, &wp_viewporter_interface, USE_VIEWPORTER_VERSION);
        check_alloc(backend->viewporter);
    } else if (strcmp(iface, zxdg_decoration_manager_v1_interface.name) == 0) {
        if (version < USE_XDG_DECORATION_VERSION) {
            ww_log(LOG_WARN, "host compositor provides outdated zxdg_decoration_manager (%d < %d)",
                   version, USE_XDG_DECORATION_VERSION);
            return;
        }

        backend->xdg_decoration_manager = wl_registry_bind(
            wl, name, &zxdg_decoration_manager_v1_interface, USE_XDG_DECORATION_VERSION);
        check_alloc(backend->xdg_decoration_manager);
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        if (version < USE_XDG_WM_BASE_VERSION) {
            ww_log(LOG_ERROR, "host compositor provides outdated xdg_wm_base (%d < %d)", version,
                   USE_XDG_WM_BASE_VERSION);
            return;
        }

        backend->xdg_wm_base =
            wl_registry_bind(wl, name, &xdg_wm_base_interface, USE_XDG_WM_BASE_VERSION);
        check_alloc(backend->xdg_wm_base);

        xdg_wm_base_add_listener(backend->xdg_wm_base, &xdg_wm_base_listener, backend);
    }
}

static void
on_registry_global_remove(void *data, struct wl_registry *wl, uint32_t name) {
    // TODO: ???
}

static const struct wl_registry_listener registry_listener = {
    .global = on_registry_global,
    .global_remove = on_registry_global_remove,
};

struct server_backend *
server_backend_create() {
    struct server_backend *backend = zalloc(1, sizeof(*backend));

    wl_list_init(&backend->seat.names);
    wl_array_init(&backend->shm_formats);

    wl_signal_init(&backend->events.seat_data_device);
    wl_signal_init(&backend->events.seat_keyboard);
    wl_signal_init(&backend->events.seat_pointer);
    wl_signal_init(&backend->events.shm_format);

    backend->display = wl_display_connect(NULL);
    if (!backend->display) {
        ww_log_errno(LOG_ERROR, "wl_display_connect failed");
        goto fail_display;
    }

    backend->registry = wl_display_get_registry(backend->display);
    check_alloc(backend->registry);
    wl_registry_add_listener(backend->registry, &registry_listener, backend);
    wl_display_roundtrip(backend->display);

    if (!backend->compositor) {
        ww_log(LOG_ERROR, "host compositor does not provide wl_compositor");
        goto fail_registry;
    }
    if (!backend->data_device_manager) {
        ww_log(LOG_ERROR, "host compositor does not provide wl_data_device_manager");
        goto fail_registry;
    }
    if (!backend->linux_dmabuf) {
        ww_log(LOG_ERROR, "host compositor does not provide zwp_linux_dmabuf");
        goto fail_registry;
    }
    if (!backend->shm) {
        ww_log(LOG_ERROR, "host compositor does not provide wl_shm");
        goto fail_registry;
    }
    if (!backend->subcompositor) {
        ww_log(LOG_ERROR, "host compositor does not provide wl_subcompositor");
        goto fail_registry;
    }
    if (!backend->viewporter) {
        ww_log(LOG_ERROR, "host compositor does not provide wp_viewporter");
        goto fail_registry;
    }
    if (!backend->xdg_wm_base) {
        ww_log(LOG_ERROR, "host compositor does not provide xdg_wm_base");
        goto fail_registry;
    }

    if (!backend->alpha_modifier) {
        ww_log(LOG_INFO, "host compositor does not provide wp_alpha_modifier");
    }
    if (!backend->cursor_shape_manager) {
        ww_log(LOG_WARN, "host compositor does not provide wp_cursor_shape_manager");
    }
    if (!backend->linux_drm_syncobj_manager) {
        ww_log(LOG_INFO, "host compositor does not provide wp_linux_drm_syncobj_manager");
    }
    if (!backend->single_pixel_buffer_manager) {
        ww_log(LOG_INFO, "host compositor does not provide wp_single_pixel_buffer_manager");
    }
    if (!backend->tearing_control) {
        ww_log(LOG_INFO, "host compositor does not provide wp_tearing_control_manager");
    }
    if (!backend->xdg_decoration_manager) {
        ww_log(LOG_WARN, "host compositor does not provide zxdg_decoration_manager");
    }

    return backend;

fail_registry:
    wl_registry_destroy(backend->registry);
    wl_display_disconnect(backend->display);

fail_display:
    wl_array_release(&backend->shm_formats);
    free(backend);
    return NULL;
}

void
server_backend_destroy(struct server_backend *backend) {
    struct seat_name *name, *tmp;
    wl_list_for_each_safe (name, tmp, &backend->seat.names, link) {
        wl_list_remove(&name->link);
        free(name);
    }

    wl_array_release(&backend->shm_formats);

    if (backend->seat.remote) {
        if (backend->seat.data_device) {
            wl_data_device_release(backend->seat.data_device);
        }
        if (backend->seat.keyboard) {
            wl_keyboard_release(backend->seat.keyboard);
        }
        if (backend->seat.pointer) {
            wl_pointer_release(backend->seat.pointer);
        }
        wl_seat_release(backend->seat.remote);
    }

    wl_compositor_destroy(backend->compositor);
    wl_data_device_manager_destroy(backend->data_device_manager);
    zwp_linux_dmabuf_v1_destroy(backend->linux_dmabuf);
    zwp_pointer_constraints_v1_destroy(backend->pointer_constraints);
    zwp_relative_pointer_manager_v1_destroy(backend->relative_pointer_manager);
    wl_shm_destroy(backend->shm);
    wl_subcompositor_destroy(backend->subcompositor);
    wp_viewporter_destroy(backend->viewporter);
    xdg_wm_base_destroy(backend->xdg_wm_base);

    if (backend->alpha_modifier) {
        wp_alpha_modifier_v1_destroy(backend->alpha_modifier);
    }
    if (backend->cursor_shape_manager) {
        wp_cursor_shape_manager_v1_destroy(backend->cursor_shape_manager);
    }
    if (backend->linux_drm_syncobj_manager) {
        wp_linux_drm_syncobj_manager_v1_destroy(backend->linux_drm_syncobj_manager);
    }
    if (backend->single_pixel_buffer_manager) {
        wp_single_pixel_buffer_manager_v1_destroy(backend->single_pixel_buffer_manager);
    }
    if (backend->tearing_control) {
        wp_tearing_control_manager_v1_destroy(backend->tearing_control);
    }
    if (backend->xdg_decoration_manager) {
        zxdg_decoration_manager_v1_destroy(backend->xdg_decoration_manager);
    }

    wl_registry_destroy(backend->registry);
    wl_display_disconnect(backend->display);

    free(backend);
}
