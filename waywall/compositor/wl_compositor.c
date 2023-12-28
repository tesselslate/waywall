#include "compositor/wl_compositor.h"
#include "compositor/buffer.h"
#include "compositor/cutil.h"
#include "compositor/server.h"
#include "compositor/xdg_shell.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "util.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <wayland-client.h>
#include <wayland-server.h>

// TODO: Figure out how to use event queues (yet again.)

/*
 *  Needed for GLFW:
 *
 *  wl_compositor.create_region
 *  wl_compositor.create_surface
 *
 *  wl_region.add
 *  wl_region.destroy
 *
 *  wl_surface.attach
 *  wl_surface.commit
 *  wl_surface.damage
 *  wl_surface.destroy
 *  wl_surface.set_buffer_scale
 *  wl_surface.set_opaque_region
 *
 *  Needed for Mesa:
 *
 *  wl_surface.attach
 *  wl_surface.commit
 *  wl_surface.damage
 *  wl_surface.damage_buffer
 *  wl_surface.destroy
 *  wl_surface.frame
 */

#define VERSION 5
#define REGION_VERSION 1

static void
on_xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
    struct server_compositor *compositor = data;

    xdg_surface_ack_configure(xdg_surface, serial);
    xdg_surface_set_window_geometry(xdg_surface, 0, 0, compositor->output.width,
                                    compositor->output.height);
    wp_viewport_set_destination(compositor->output.root_viewport, compositor->output.width,
                                compositor->output.height);
    wl_surface_commit(compositor->output.root_surface);

    // TODO: emit resize event
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = on_xdg_surface_configure,
};

static void
on_xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel, int32_t width,
                          int32_t height, struct wl_array *states) {
    struct server_compositor *compositor = data;

    width = width > 0 ? width : compositor->output.width > 0 ? compositor->output.width : 640;
    height = height > 0 ? height : compositor->output.height > 0 ? compositor->output.height : 480;

    compositor->output.width = width;
    compositor->output.height = height;
}

static void
on_xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel) {
    struct server_compositor *compositor = data;

    server_compositor_hide_window(compositor);

    // TODO: show popup or something to user maybe
}

static void
on_xdg_toplevel_configure_bounds(void *data, struct xdg_toplevel *xdg_toplevel, int32_t width,
                                 int32_t height) {
    // Unused. We do not do any CSD.
}

static void
on_xdg_toplevel_wm_capabilities(void *data, struct xdg_toplevel *xdg_toplevel,
                                struct wl_array *capabilities) {
    // Unused.
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = on_xdg_toplevel_configure,
    .close = on_xdg_toplevel_close,
    .configure_bounds = on_xdg_toplevel_configure_bounds,
    .wm_capabilities = on_xdg_toplevel_wm_capabilities,
};

static void
on_toplevel_unmap(struct wl_listener *listener, void *data) {
    struct server_view *view = wl_container_of(listener, view, on_unmap);

    server_view_destroy(view);
}

static void
on_role_object_destroy(struct wl_listener *listener, void *data) {
    struct server_surface *surface = wl_container_of(listener, surface, on_role_object_destroy);

    surface->role_object = NULL;
    wl_list_remove(&surface->on_role_object_destroy.link);
}

static void
on_frame_callback_done(void *data, struct wl_callback *wl_callback, uint32_t time) {
    struct wl_resource *callback_resource = data;
    wl_callback_send_done(callback_resource, time);
}

static const struct wl_callback_listener frame_callback_listener = {
    .done = on_frame_callback_done,
};

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_compositor *compositor = wl_container_of(listener, compositor, display_destroy);

    xdg_toplevel_destroy(compositor->output.xdg_toplevel);
    xdg_surface_destroy(compositor->output.xdg_surface);
    wp_viewport_destroy(compositor->output.root_viewport);
    wl_surface_destroy(compositor->output.root_surface);
    wl_buffer_destroy(compositor->output.background);

    wl_subcompositor_destroy(compositor->output.subcompositor);
    wp_single_pixel_buffer_manager_v1_destroy(compositor->output.single_pixel_buffer_manager);
    wp_viewporter_destroy(compositor->output.viewporter);
    xdg_wm_base_destroy(compositor->output.xdg_wm_base);

    wl_compositor_destroy(compositor->remote);
    wl_global_destroy(compositor->global);

    free(compositor);
}

static void
frame_callback_destroy(struct wl_resource *resource) {
    struct wl_callback *callback = wl_resource_get_user_data(resource);
    wl_callback_destroy(callback);
}

static void
handle_region_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_region_add(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y,
                  int32_t width, int32_t height) {
    struct server_region *region = server_region_from_resource(resource);

    wl_region_add(region->remote, x, y, width, height);
}

static void
handle_region_subtract(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y,
                       int32_t width, int32_t height) {
    struct server_region *region = server_region_from_resource(resource);

    wl_region_subtract(region->remote, x, y, width, height);
}

static void
region_destroy(struct wl_resource *resource) {
    struct server_region *region = server_region_from_resource(resource);

    wl_region_destroy(region->remote);
    free(region);
}

static const struct wl_region_interface region_impl = {
    .destroy = handle_region_destroy,
    .add = handle_region_add,
    .subtract = handle_region_subtract,
};

struct server_region *
server_region_from_resource(struct wl_resource *resource) {
    ww_assert(wl_resource_instance_of(resource, &wl_region_interface, &region_impl));
    return wl_resource_get_user_data(resource);
}

static void
handle_surface_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_surface_attach(struct wl_client *client, struct wl_resource *resource,
                      struct wl_resource *buffer_resource, int32_t x, int32_t y) {
    struct server_surface *surface = server_surface_from_resource(resource);
    struct server_buffer *buffer = server_buffer_from_resource(buffer_resource);

    if (x != 0 || y != 0) {
        if (wl_resource_get_version(resource) >= WL_SURFACE_OFFSET_SINCE_VERSION) {
            wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_OFFSET,
                                   "non-zero offset provided in wl_surface.attach");
        } else {
            wl_client_post_implementation_error(client, "non-zero surface offset not allowed");
        }
        return;
    }

    surface->pending.buffer = buffer;
    surface->pending.changes |= SURFACE_STATE_BUFFER;
}

static void
handle_surface_damage(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y,
                      int32_t width, int32_t height) {
    struct server_surface *surface = server_surface_from_resource(resource);

    struct box *damage = wl_array_add(&surface->pending.damage, sizeof(*damage));
    damage->x = x;
    damage->y = y;
    damage->width = width;
    damage->height = height;
    surface->pending.changes |= SURFACE_STATE_DAMAGE;
}

static void
handle_surface_frame(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_surface *surface = server_surface_from_resource(resource);

    struct wl_callback *callback = wl_surface_frame(surface->remote);

    struct wl_resource *callback_resource =
        wl_resource_create(client, &wl_callback_interface, 1, id);
    if (!callback_resource) {
        wl_client_post_no_memory(client);
        return;
    }

    wl_resource_set_implementation(callback_resource, NULL, callback, frame_callback_destroy);

    wl_callback_add_listener(callback, &frame_callback_listener, callback_resource);
}

static void
handle_surface_set_opaque_region(struct wl_client *client, struct wl_resource *resource,
                                 struct wl_resource *region_resource) {
    // Unused. This might be worth implementing at some point but dealing with wl_region's copy
    // semantics is annoying; we can't just store the server_region on its own (a refcount could
    // work, or maybe I just need to give up and use pixman.)
}

static void
handle_surface_set_input_region(struct wl_client *client, struct wl_resource *resource,
                                struct wl_resource *region_resource) {
    // We do not want to give clients control over the input regions of their surfaces. We want
    // to disable input on all of their surfaces so that all input events are passed through to
    // the remote parent surface.

    // No relevant clients make use of this function.
    wl_client_post_implementation_error(client, "wl_surface.set_input_region is not implemented");
}

static void
handle_surface_commit(struct wl_client *client, struct wl_resource *resource) {
    struct server_surface *surface = server_surface_from_resource(resource);

    wl_signal_emit_mutable(&surface->events.commit, surface);

    if (surface->pending.changes & SURFACE_STATE_BUFFER) {
        surface->current_buffer = surface->pending.buffer;
        wl_surface_attach(surface->remote, surface->pending.buffer->remote, 0, 0);
    }
    if (surface->pending.changes & SURFACE_STATE_DAMAGE) {
        struct box *damage;
        wl_array_for_each(damage, &surface->pending.damage) {
            wl_surface_damage(surface->remote, damage->x, damage->y, damage->width, damage->height);
        }
    }
    if (surface->pending.changes & SURFACE_STATE_BUFFER_DAMAGE) {
        struct box *damage;
        wl_array_for_each(damage, &surface->pending.buffer_damage) {
            wl_surface_damage_buffer(surface->remote, damage->x, damage->y, damage->width,
                                     damage->height);
        }
    }
    if (surface->pending.changes & SURFACE_STATE_SCALE) {
        wl_surface_set_buffer_scale(surface->remote, surface->pending.scale);
    }
    wl_surface_commit(surface->remote);

    wl_array_release(&surface->pending.damage);
    wl_array_release(&surface->pending.buffer_damage);

    surface->pending = (struct surface_client_state){0};
    wl_array_init(&surface->pending.damage);
    wl_array_init(&surface->pending.buffer_damage);
}

static void
handle_surface_set_buffer_transform(struct wl_client *client, struct wl_resource *resource,
                                    int32_t transform) {
    // No relevant clients make use of this function.
    wl_client_post_implementation_error(client,
                                        "wl_surface.set_buffer_transform is not implemented");
}

static void
handle_surface_set_buffer_scale(struct wl_client *client, struct wl_resource *resource,
                                int32_t scale) {
    struct server_surface *surface = server_surface_from_resource(resource);

    // TODO: check that buffer scale is legal (kill client early instead of getting killed by host
    // compositor)

    surface->pending.scale = scale;
    surface->pending.changes |= SURFACE_STATE_SCALE;
}

static void
handle_surface_damage_buffer(struct wl_client *client, struct wl_resource *resource, int32_t x,
                             int32_t y, int32_t width, int32_t height) {
    struct server_surface *surface = server_surface_from_resource(resource);

    struct box *damage = wl_array_add(&surface->pending.buffer_damage, sizeof(*damage));
    damage->x = x;
    damage->y = y;
    damage->width = width;
    damage->height = height;
    surface->pending.changes |= SURFACE_STATE_BUFFER_DAMAGE;
}

static void
handle_surface_offset(struct wl_client *client, struct wl_resource *resource, int32_t x,
                      int32_t y) {
    // No relevant clients make use of this function.
    wl_client_post_implementation_error(client, "wl_surface.offset is not implemented");
}

static void
surface_destroy(struct wl_resource *resource) {
    struct server_surface *surface = server_surface_from_resource(resource);

    wl_signal_emit_mutable(&surface->events.destroy, surface);

    wl_array_release(&surface->pending.damage);
    wl_array_release(&surface->pending.buffer_damage);

    if (surface->role_object) {
        struct wl_listener *listener =
            wl_resource_get_destroy_listener(surface->role_object, on_role_object_destroy);
        ww_assert(listener == &surface->on_role_object_destroy);

        wl_list_remove(&surface->on_role_object_destroy.link);
    }

    wl_surface_destroy(surface->remote);
    free(surface);
}

static const struct wl_surface_interface surface_impl = {
    .destroy = handle_surface_destroy,
    .attach = handle_surface_attach,
    .damage = handle_surface_damage,
    .frame = handle_surface_frame,
    .set_opaque_region = handle_surface_set_opaque_region,
    .set_input_region = handle_surface_set_input_region,
    .commit = handle_surface_commit,
    .set_buffer_transform = handle_surface_set_buffer_transform,
    .set_buffer_scale = handle_surface_set_buffer_scale,
    .damage_buffer = handle_surface_damage_buffer,
    .offset = handle_surface_offset,
};

struct server_surface *
server_surface_from_resource(struct wl_resource *resource) {
    ww_assert(wl_resource_instance_of(resource, &wl_surface_interface, &surface_impl));
    return wl_resource_get_user_data(resource);
}

static void
handle_compositor_create_region(struct wl_client *client, struct wl_resource *resource,
                                uint32_t id) {
    struct server_compositor *compositor = server_compositor_from_resource(resource);

    struct wl_resource *region_resource =
        wl_resource_create(client, &wl_region_interface, REGION_VERSION, id);
    if (!region_resource) {
        wl_client_post_no_memory(client);
        return;
    }

    struct server_region *region = calloc(1, sizeof(*region));
    if (!region) {
        wl_client_post_no_memory(client);
        return;
    }

    region->remote = wl_compositor_create_region(compositor->remote);

    wl_resource_set_implementation(region_resource, &region_impl, region, region_destroy);

    region->resource = region_resource;
}

static void
handle_compositor_create_surface(struct wl_client *client, struct wl_resource *resource,
                                 uint32_t id) {
    struct server_compositor *compositor = server_compositor_from_resource(resource);

    struct wl_resource *surface_resource =
        wl_resource_create(client, &wl_surface_interface, wl_resource_get_version(resource), id);
    if (!surface_resource) {
        wl_client_post_no_memory(client);
        return;
    }

    struct server_surface *surface = calloc(1, sizeof(*surface));
    if (!surface) {
        wl_client_post_no_memory(client);
        return;
    }

    wl_signal_init(&surface->events.commit);
    wl_signal_init(&surface->events.destroy);

    surface->remote = wl_compositor_create_surface(compositor->remote);
    wl_array_init(&surface->pending.damage);
    wl_array_init(&surface->pending.buffer_damage);

    struct wl_region *input_region = wl_compositor_create_region(compositor->remote);
    wl_surface_set_input_region(surface->remote, input_region);
    wl_region_destroy(input_region);

    wl_resource_set_implementation(surface_resource, &surface_impl, surface, surface_destroy);

    surface->resource = surface_resource;
}

static void
compositor_destroy(struct wl_resource *resource) {
    // Unused.
}

static const struct wl_compositor_interface compositor_impl = {
    .create_region = handle_compositor_create_region,
    .create_surface = handle_compositor_create_surface,
};

struct server_compositor *
server_compositor_from_resource(struct wl_resource *resource) {
    ww_assert(wl_resource_instance_of(resource, &wl_compositor_interface, &compositor_impl));
    return wl_resource_get_user_data(resource);
}

static void
handle_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= VERSION);
    struct server_compositor *compositor = data;

    struct wl_resource *resource =
        wl_resource_create(client, &wl_compositor_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }

    wl_resource_set_implementation(resource, &compositor_impl, compositor, compositor_destroy);
}

struct server_compositor *
server_compositor_create(struct server *server, struct wl_compositor *remote) {
    struct server_compositor *compositor = calloc(1, sizeof(*compositor));
    if (!compositor) {
        LOG(LOG_ERROR, "failed to allocate server_compositor");
        return NULL;
    }

    compositor->remote = remote;
    compositor->output.remote_display = server->remote.display;
    compositor->output.subcompositor = server->remote.subcompositor;
    compositor->output.single_pixel_buffer_manager = server->remote.single_pixel_buffer_manager;
    compositor->output.viewporter = server->remote.viewporter;
    compositor->output.xdg_wm_base = server->remote.xdg_wm_base;
    compositor->global = wl_global_create(server->display, &wl_compositor_interface, VERSION,
                                          compositor, handle_bind);

    compositor->display_destroy.notify = on_display_destroy;

    wl_display_add_destroy_listener(server->display, &compositor->display_destroy);

    // TODO: allow configuring background color
    compositor->output.background = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
        compositor->output.single_pixel_buffer_manager, UINT32_MAX, UINT32_MAX, UINT32_MAX,
        UINT32_MAX);
    compositor->output.root_surface = wl_compositor_create_surface(compositor->remote);
    compositor->output.root_viewport =
        wp_viewporter_get_viewport(compositor->output.viewporter, compositor->output.root_surface);

    compositor->output.xdg_surface = xdg_wm_base_get_xdg_surface(compositor->output.xdg_wm_base,
                                                                 compositor->output.root_surface);
    xdg_surface_add_listener(compositor->output.xdg_surface, &xdg_surface_listener, compositor);
    compositor->output.xdg_toplevel = xdg_surface_get_toplevel(compositor->output.xdg_surface);
    xdg_toplevel_add_listener(compositor->output.xdg_toplevel, &xdg_toplevel_listener, compositor);

    server_compositor_show_window(compositor);

    wl_list_init(&compositor->output.views);

    return compositor;
}

void
server_compositor_hide_window(struct server_compositor *compositor) {
    ww_assert(compositor->output.mapped);

    wl_surface_attach(compositor->output.root_surface, NULL, 0, 0);
    wl_surface_commit(compositor->output.root_surface);

    compositor->output.mapped = false;
}

void
server_compositor_show_window(struct server_compositor *compositor) {
    ww_assert(!compositor->output.mapped);

    wl_surface_attach(compositor->output.root_surface, NULL, 0, 0);
    wl_surface_commit(compositor->output.root_surface);
    wl_display_roundtrip(compositor->output.remote_display);

    wl_surface_attach(compositor->output.root_surface, compositor->output.background, 0, 0);
    wl_surface_commit(compositor->output.root_surface);
    wl_display_roundtrip(compositor->output.remote_display);

    compositor->output.mapped = true;
}

void
server_surface_set_role_object(struct server_surface *surface, struct wl_resource *resource) {
    if (surface->role_object) {
        struct wl_listener *listener =
            wl_resource_get_destroy_listener(surface->role_object, on_role_object_destroy);
        ww_assert(listener == &surface->on_role_object_destroy);

        wl_list_remove(&surface->on_role_object_destroy.link);
    }

    surface->role_object = resource;

    surface->on_role_object_destroy.notify = on_role_object_destroy;
    wl_resource_add_destroy_listener(resource, &surface->on_role_object_destroy);
}

struct server_view *
server_view_create_toplevel(struct server_compositor *compositor,
                            struct server_xdg_toplevel *toplevel) {
    ww_assert(!server_view_from_toplevel(compositor, toplevel));

    struct server_view *view = calloc(1, sizeof(*view));
    if (!view) {
        LOG(LOG_ERROR, "failed to allocate server_view");
        return NULL;
    }

    view->surface = toplevel->parent->parent;
    view->subsurface = wl_subcompositor_get_subsurface(
        compositor->output.subcompositor, view->surface->remote, compositor->output.root_surface);
    view->viewport =
        wp_viewporter_get_viewport(compositor->output.viewporter, view->surface->remote);
    view->data.xdg_shell = toplevel;
    view->type = VIEW_XDG_SHELL;

    // Place the view below the background until it is configured properly by us.
    wl_subsurface_place_below(view->subsurface, compositor->output.root_surface);
    wl_subsurface_set_desync(view->subsurface);

    view->on_unmap.notify = on_toplevel_unmap;
    wl_signal_add(&toplevel->events.unmap, &view->on_unmap);

    wl_signal_init(&view->events.destroy);

    wl_list_insert(&compositor->output.views, &view->link);

    return view;
}

struct server_view *
server_view_from_toplevel(struct server_compositor *compositor,
                          struct server_xdg_toplevel *toplevel) {
    struct server_view *view;
    wl_list_for_each (view, &compositor->output.views, link) {
        if (view->type == VIEW_XDG_SHELL && view->data.xdg_shell == toplevel) {
            return view;
        }
    }

    return NULL;
}

void
server_view_destroy(struct server_view *view) {
    wl_signal_emit_mutable(&view->events.destroy, view);

    wp_viewport_destroy(view->viewport);
    wl_subsurface_destroy(view->subsurface);

    wl_list_remove(&view->on_unmap.link);
    wl_list_remove(&view->link);

    free(view);
}

struct server_surface *
server_view_get_surface(struct server_view *view) {
    switch (view->type) {
    case VIEW_XDG_SHELL:
        return view->data.xdg_shell->parent->parent;
    default:
        ww_unreachable();
    }
}

void
server_view_place_above(struct server_view *view, struct wl_surface *sibling) {
    wl_subsurface_place_above(view->subsurface, sibling);
    wl_surface_commit(view->surface->remote);
}

void
server_view_place_below(struct server_view *view, struct wl_surface *sibling) {
    wl_subsurface_place_below(view->subsurface, sibling);
    wl_surface_commit(view->surface->remote);
}

void
server_view_set_pos(struct server_view *view, int32_t x, int32_t y) {
    wl_subsurface_set_position(view->subsurface, x, y);
    wl_surface_commit(view->surface->remote);

    view->bounds.x = x;
    view->bounds.y = y;
}

void
server_view_set_size(struct server_view *view, int32_t width, int32_t height) {
    wp_viewport_set_destination(view->viewport, width, height);
    wl_surface_commit(view->surface->remote);

    view->bounds.width = width;
    view->bounds.height = height;
}

void
server_view_set_source(struct server_view *view, double x, double y, double width, double height) {
    wp_viewport_set_source(view->viewport, wl_fixed_from_double(x), wl_fixed_from_double(y),
                           wl_fixed_from_double(width), wl_fixed_from_double(height));
    wl_surface_commit(view->surface->remote);
}
