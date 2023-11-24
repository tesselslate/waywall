/*
    This file implements the "compositor" portion of waywall. Compositor is in
    quotes because we do no compositing ourselves.

    The waywall "compositor" largely acts as an intermediary between Wayland clients
    (Minecraft instances) and the user's host Wayland compositor, doing some
    additional processing or tampering with information as needed.

    The most notable thing, as mentioned above, is that we do no compositing
    ourselves. Clients can ask for DMABUF-based wl_buffers via the linux_dmabuf
    protocol, which we implement by forwarding requests to the host compositor.
    Then, we "composite" them by making use of wl_subcompositor to position
    several buffers on screen in whatever arrangement we like. The host compositor
    still has to do the heavy lifting.

    This method of implementing a "compositor" simplifies things a lot because we
    let the user's host compositor deal with all of the hard problems (like the
    actual compositing.) However, it comes at the cost of reliability and
    portability, since an error from any Wayland client can bring down the whole
    waywall "compositor" and we cannot run nested in X11 environments, only Wayland.

    In addition, a lot of things have been left unimplemented or have been
    implemented in a way which is not compliant with the specification. waywall's
    intended use case is very narrow, and this is intentional.

    -------------------------------------------------------------------------------

    Some code in this file is adapted from or based off of wlroots' code.
    wlroots' LICENSE contents are included below.

    Copyright (c) 2017, 2018 Drew DeVault
    Copyright (c) 2014 Jari Vetoniemi
    Copyright (c) 2023 The wlroots contributors

    Permission is hereby granted, free of charge, to any person obtaining a copy of
    this software and associated documentation files (the "Software"), to deal in
    the Software without restriction, including without limitation the rights to
    use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
    of the Software, and to permit persons to whom the Software is furnished to do
    so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

    -------------------------------------------------------------------------------
 */

// TODO: use instance stretch height by default
#define WINDOW_WIDTH 160
#define WINDOW_HEIGHT 160

#include "compositor.h"
#include "util.h"
#include <signal.h>
#include <stdlib.h>
#include <wayland-client.h>
#include <wayland-server.h>

// Client protocols
#include "linux-dmabuf-unstable-v1-protocol.h"
#include "pointer-constraints-unstable-v1-protocol.h"
#include "relative-pointer-unstable-v1-protocol.h"
#include "single-pixel-buffer-v1-protocol.h"
#include "tearing-control-v1-protocol.h"
#include "viewporter-protocol.h"
#include "xdg-shell-protocol.h"

// Server protocols
#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "pointer-constraints-unstable-v1-server-protocol.h"
#include "relative-pointer-unstable-v1-server-protocol.h"
#include "tearing-control-v1-server-protocol.h"
#include "xdg-decoration-unstable-v1-server-protocol.h"
#include "xdg-shell-server-protocol.h"

#define RINGBUF_SIZE 64

struct ringbuf {
    uint32_t data[RINGBUF_SIZE];
    size_t head, tail, count;
};

static inline uint32_t
ringbuf_at(struct ringbuf *ringbuf, size_t index) {
    ww_assert(ringbuf->count > index);

    return ringbuf->data[(ringbuf->head + index) % RINGBUF_SIZE];
}

static inline void
ringbuf_pop_n(struct ringbuf *ringbuf, size_t num) {
    ww_assert(ringbuf->count >= num);

    ringbuf->count -= num;
    ringbuf->head += num;
    ringbuf->head %= RINGBUF_SIZE;
}

static bool
ringbuf_push(struct ringbuf *ringbuf, uint32_t datum) {
    if (ringbuf->count == RINGBUF_SIZE) {
        return false;
    }

    ringbuf->data[ringbuf->tail] = datum;

    ringbuf->count++;
    ringbuf->tail++;
    ringbuf->tail %= RINGBUF_SIZE;

    return true;
}

struct client_seat {
    struct wl_list link; // compositor_remote_wl.seats

    struct compositor *compositor;

    struct wl_seat *wl;
    uint32_t caps;
    uint32_t name;
    char *str_name;

    struct wl_pointer *pointer;
    struct wl_resource *pointer_focus;
    struct zwp_relative_pointer_v1 *relative_pointer;

    struct wl_keyboard *keyboard;
    struct wl_resource *keyboard_focus;
    uint32_t pressed_keys[128];
    uint32_t num_pressed_keys;
    uint32_t mods_depressed, mods_latched, mods_locked, group;

    // Server state (associated wl_seat global)
    struct wl_global *global;
    struct wl_list clients;
};

struct dmabuf {
    struct wl_list link; // server_buffer_params.dmabuf

    int fd;
    uint32_t plane_idx, offset, stride;
    uint64_t modifier;
};

struct server_buffer {
    struct wl_buffer *buffer;

    enum {
        BUFFER_SHM,
        BUFFER_DMABUF,
    } type;
    union {
        struct {
            int32_t offset, width, height, stride;
            uint32_t format;
        } shm;
        struct {
            int32_t width, height;
            uint32_t format, flags;
            struct wl_list bufs; // dmabuf.link
        } dmabuf;
    } data;
};

struct server_xdg_surface {
    struct server_surface *server_surface;

    struct wl_client *client;
    struct wl_resource *toplevel_resource;

    struct ringbuf configure_serials;
    bool acked;

    int32_t width, height;
    bool fullscreen;
};

struct server_xdg_toplevel {
    struct server_xdg_surface *server_xdg_surface;

    struct wl_resource *decoration_resource;
    int32_t min_width, min_height, max_width, max_height;
};

struct server_seat {
    struct wl_resource *resource;
    uint32_t version;

    struct client_seat *client_seat;
    uint32_t caps, past_caps;

    struct wl_list pointers, keyboards;
};

struct server_pointer {
    struct server_seat *server_seat;

    struct wl_list relative_pointers;
};

static void destroy_remote_window(struct compositor *compositor);
static void send_xdg_surface_configure(struct wl_resource *xdg_surface_resource);
static void send_xdg_toplevel_configure(struct wl_resource *xdg_toplevel_resource);

static inline int32_t
server_buffer_get_width(struct server_buffer *buffer) {
    switch (buffer->type) {
    case BUFFER_SHM:
        return buffer->data.shm.width;
    case BUFFER_DMABUF:
        return buffer->data.dmabuf.width;
    default:
        ww_unreachable();
    }
}

static inline int32_t
server_buffer_get_height(struct server_buffer *buffer) {
    switch (buffer->type) {
    case BUFFER_SHM:
        return buffer->data.shm.height;
    case BUFFER_DMABUF:
        return buffer->data.dmabuf.width;
    default:
        ww_unreachable();
    }
}

/*
 *  Server code
 */

static inline uint32_t
next_serial(struct wl_resource *resource) {
    return wl_display_next_serial(wl_client_get_display(wl_resource_get_client(resource)));
}

static inline uint32_t
current_time() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t ms = (uint64_t)(now.tv_sec * 1000) + (uint64_t)(now.tv_nsec / 1000000);
    return (uint32_t)ms;
}

#define SERVER_LINUX_DMABUF_VERSION 4
#define SERVER_WL_CALLBACK_VERSION 1
#define SERVER_WL_COMPOSITOR_VERSION 6
#define SERVER_WL_OUTPUT_VERSION 4
#define SERVER_WL_SEAT_VERSION 9
#define SERVER_WL_SHM_VERSION 1
#define SERVER_RELATIVE_POINTER_VERSION 1
#define SERVER_XDG_DECORATION_VERSION 1
#define SERVER_XDG_WM_BASE_VERSION 6

static void
handle_add_wl_region(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y,
                     int32_t width, int32_t height) {
    struct wl_region *region = wl_resource_get_user_data(resource);
    wl_region_add(region, x, y, width, height);
}

static void
handle_destroy_wl_region(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_subtract_wl_region(struct wl_client *client, struct wl_resource *resource, int32_t x,
                          int32_t y, int32_t width, int32_t height) {
    struct wl_region *region = wl_resource_get_user_data(resource);
    wl_region_subtract(region, x, y, width, height);
}

static void
destroy_wl_region(struct wl_resource *resource) {
    struct wl_region *region = wl_resource_get_user_data(resource);
    wl_region_destroy(region);
}

static const struct wl_region_interface wl_region_implementation = {
    .add = handle_add_wl_region,
    .destroy = handle_destroy_wl_region,
    .subtract = handle_subtract_wl_region,
};

struct server_surface {
    struct compositor *compositor;

    struct wl_resource *buffer_resource;
    int buffer_scale;

    struct wl_surface *surface;
    struct wl_subsurface *subsurface;

    struct wl_list frame_callbacks;

    enum {
        ROLE_NONE = 0,
        ROLE_XDG,
        ROLE_CURSOR,
    } role;
    struct wl_resource *xdg_resource;
};

static void
destroy_server_surface_frame_callback(struct wl_resource *resource) {
    struct wl_callback *remote_cb = wl_resource_get_user_data(resource);
    wl_callback_destroy(remote_cb);
    wl_list_remove(wl_resource_get_link(resource));
}

static void
on_surface_frame_done(void *data, struct wl_callback *callback, uint32_t time) {
    struct wl_resource *cb_resource = data;
    wl_callback_send_done(cb_resource, current_time());
    wl_resource_destroy(cb_resource);
}

static const struct wl_callback_listener surface_frame_listener = {
    .done = on_surface_frame_done,
};

static void
handle_destroy_wl_surface(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_attach_wl_surface(struct wl_client *client, struct wl_resource *resource,
                         struct wl_resource *buffer_resource, int32_t x, int32_t y) {
    struct server_surface *server_surface = wl_resource_get_user_data(resource);
    struct server_buffer *server_buffer = wl_resource_get_user_data(buffer_resource);

    if (server_buffer_get_width(server_buffer) % server_surface->buffer_scale != 0) {
        wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_SIZE,
                               "width not multiple of buffer scale");
        return;
    }
    if (server_buffer_get_height(server_buffer) % server_surface->buffer_scale != 0) {
        wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_SIZE,
                               "height not multiple of buffer scale");
    }
    if (wl_resource_get_version(resource) >= WL_SURFACE_OFFSET_SINCE_VERSION &&
        (x != 0 || y != 0)) {
        wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_OFFSET,
                               "non-zero offset provided in wl_surface.attach");
        return;
    }

    if (server_surface->xdg_resource) {
        struct server_xdg_surface *server_xdg_surface =
            wl_resource_get_user_data(server_surface->xdg_resource);
        if (!server_xdg_surface->acked) {
            wl_resource_post_error(server_surface->xdg_resource,
                                   XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
                                   "wl_surface.attach called before xdg_surface.ack_configure");
            return;
        }
    }

    server_surface->buffer_resource = buffer_resource;

    if (!buffer_resource) {
        wl_surface_attach(server_surface->surface, NULL, 0, 0);
        return;
    }

    wl_surface_attach(server_surface->surface, server_buffer->buffer, 0, 0);
}

static void
handle_damage_wl_surface(struct wl_client *client, struct wl_resource *resource, int32_t x,
                         int32_t y, int32_t width, int32_t height) {
    struct server_surface *server_surface = wl_resource_get_user_data(resource);

    wl_surface_damage(server_surface->surface, x, y, width, height);
}

static void
handle_frame_wl_surface(struct wl_client *client, struct wl_resource *surface_resource,
                        uint32_t id) {
    struct server_surface *server_surface = wl_resource_get_user_data(surface_resource);

    struct wl_callback *remote_cb = wl_surface_frame(server_surface->surface);
    struct wl_resource *resource =
        wl_resource_create(client, &wl_callback_interface, SERVER_WL_CALLBACK_VERSION, id);
    wl_resource_set_implementation(resource, NULL, remote_cb,
                                   destroy_server_surface_frame_callback);

    wl_list_insert(&server_surface->frame_callbacks, wl_resource_get_link(resource));

    wl_callback_add_listener(remote_cb, &surface_frame_listener, resource);
}

static void
handle_set_opaque_region_wl_surface(struct wl_client *client, struct wl_resource *resource,
                                    struct wl_resource *region_resource) {
    struct server_surface *server_surface = wl_resource_get_user_data(resource);

    if (!region_resource) {
        wl_surface_set_opaque_region(server_surface->surface, NULL);
        return;
    }

    struct wl_region *region = wl_resource_get_user_data(region_resource);
    wl_surface_set_opaque_region(server_surface->surface, region);
}

static void
handle_set_input_region_wl_surface(struct wl_client *client, struct wl_resource *resource,
                                   struct wl_resource *region_resource) {
    struct server_surface *server_surface = wl_resource_get_user_data(resource);

    if (!region_resource) {
        wl_surface_set_input_region(server_surface->surface, NULL);
        return;
    }

    struct wl_region *region = wl_resource_get_user_data(region_resource);
    wl_surface_set_input_region(server_surface->surface, region);
}

static void
handle_commit_wl_surface(struct wl_client *client, struct wl_resource *resource) {
    struct server_surface *server_surface = wl_resource_get_user_data(resource);

    if (server_surface->xdg_resource) {
        struct server_xdg_surface *server_xdg_surface =
            wl_resource_get_user_data(server_surface->xdg_resource);

        if (!server_surface->buffer_resource) {
            if (server_xdg_surface->toplevel_resource) {
                send_xdg_toplevel_configure(server_xdg_surface->toplevel_resource);
            }

            send_xdg_surface_configure(server_surface->xdg_resource);
        } else {
            // TODO: we are supposed to check if the client acked the latest xdg_surface.configure
        }
    }

    wl_surface_commit(server_surface->surface);
}

static void
handle_set_buffer_transform_wl_surface(struct wl_client *client, struct wl_resource *resource,
                                       int32_t transform) {
    struct server_surface *server_surface = wl_resource_get_user_data(resource);

    if (transform < WL_OUTPUT_TRANSFORM_NORMAL || transform > WL_OUTPUT_TRANSFORM_FLIPPED_270) {
        wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_TRANSFORM,
                               "provided transform (%" PRIi32 ") does not exist", transform);
        return;
    }

    wl_surface_set_buffer_transform(server_surface->surface, transform);
}

static void
handle_set_buffer_scale_wl_surface(struct wl_client *client, struct wl_resource *resource,
                                   int32_t scale) {
    struct server_surface *server_surface = wl_resource_get_user_data(resource);

    if (scale <= 0) {
        wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_SCALE,
                               "provided scale (%" PRIi32 ") is not positive", scale);
        return;
    }

    server_surface->buffer_scale = scale;
    wl_surface_set_buffer_scale(server_surface->surface, scale);
}

static void
handle_damage_buffer_wl_surface(struct wl_client *client, struct wl_resource *resource, int32_t x,
                                int32_t y, int32_t width, int32_t height) {
    struct server_surface *server_surface = wl_resource_get_user_data(resource);

    wl_surface_damage_buffer(server_surface->surface, x, y, width, height);
}

static void
handle_offset_wl_surface(struct wl_client *client, struct wl_resource *resource, int32_t x,
                         int32_t y) {
    struct server_surface *server_surface = wl_resource_get_user_data(resource);

    wl_surface_offset(server_surface->surface, x, y);
}

static void
destroy_wl_surface(struct wl_resource *resource) {
    struct server_surface *server_surface = wl_resource_get_user_data(resource);

    wl_surface_destroy(server_surface->surface);

    struct wl_resource *cb_resource, *tmp;
    wl_resource_for_each_safe(cb_resource, tmp, &server_surface->frame_callbacks) {
        wl_resource_destroy(cb_resource);
    }

    if (server_surface->xdg_resource) {
        wl_resource_destroy(server_surface->xdg_resource);
    }

    wl_list_remove(wl_resource_get_link(resource));
    free(server_surface);
}

static const struct wl_surface_interface wl_surface_implementation = {
    .destroy = handle_destroy_wl_surface,
    .attach = handle_attach_wl_surface,
    .damage = handle_damage_wl_surface,
    .frame = handle_frame_wl_surface,
    .set_opaque_region = handle_set_opaque_region_wl_surface,
    .set_input_region = handle_set_input_region_wl_surface,
    .commit = handle_commit_wl_surface,
    .set_buffer_transform = handle_set_buffer_transform_wl_surface,
    .set_buffer_scale = handle_set_buffer_scale_wl_surface,
    .damage_buffer = handle_damage_buffer_wl_surface,
    .offset = handle_offset_wl_surface,
};

static void
handle_create_region_wl_compositor(struct wl_client *client, struct wl_resource *resource,
                                   uint32_t id) {
    struct compositor *compositor = wl_resource_get_user_data(resource);
    struct wl_region *region = wl_compositor_create_region(compositor->remote.compositor);

    struct wl_resource *region_resource =
        wl_resource_create(client, &wl_region_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(region_resource, &wl_region_implementation, region,
                                   destroy_wl_region);
}

static void
handle_create_surface_wl_compositor(struct wl_client *client, struct wl_resource *resource,
                                    uint32_t id) {
    struct compositor *compositor = wl_resource_get_user_data(resource);
    struct server_surface *server_surface = ww_alloc(1, sizeof(*server_surface));

    struct wl_resource *surface_resource =
        wl_resource_create(client, &wl_surface_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(surface_resource, &wl_surface_implementation, server_surface,
                                   destroy_wl_surface);

    server_surface->compositor = compositor;
    server_surface->surface = wl_compositor_create_surface(compositor->remote.compositor);
    wl_surface_set_user_data(server_surface->surface, surface_resource);
    server_surface->buffer_scale = 1;
    wl_list_init(&server_surface->frame_callbacks);

    wl_list_insert(&compositor->globals.surfaces, wl_resource_get_link(surface_resource));
}

static void
destroy_wl_compositor(struct wl_resource *resource) {
    // Unused.
}

static const struct wl_compositor_interface wl_compositor_implementation = {
    .create_region = handle_create_region_wl_compositor,
    .create_surface = handle_create_surface_wl_compositor,
};

static void
handle_bind_wl_compositor(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SERVER_WL_COMPOSITOR_VERSION);
    struct compositor *compositor = data;

    struct wl_resource *resource =
        wl_resource_create(client, &wl_compositor_interface, wl_compositor_interface.version, id);
    wl_resource_set_implementation(resource, &wl_compositor_implementation, compositor,
                                   destroy_wl_compositor);
}

static void
handle_destroy_relative_pointer(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
destroy_relative_pointer(struct wl_resource *resource) {
    struct server_pointer *server_pointer = wl_resource_get_user_data(resource);

    if (server_pointer) {
        wl_list_remove(wl_resource_get_link(resource));
    }
}

static const struct zwp_relative_pointer_v1_interface relative_pointer_implementation = {
    .destroy = handle_destroy_relative_pointer,
};

static void
handle_destroy_relative_pointer_manager(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_get_relative_pointer_relative_pointer_manager(struct wl_client *client,
                                                     struct wl_resource *resource, uint32_t id,
                                                     struct wl_resource *pointer_resource) {
    struct server_pointer *server_pointer = wl_resource_get_user_data(pointer_resource);

    struct wl_resource *relative_pointer_resource = wl_resource_create(
        client, &zwp_relative_pointer_v1_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(relative_pointer_resource, &relative_pointer_implementation,
                                   server_pointer, destroy_relative_pointer);
}

static void
destroy_relative_pointer_manager(struct wl_resource *resource) {
    // Unused.
}

static const struct zwp_relative_pointer_manager_v1_interface
    relative_pointer_manager_implementation = {
        .destroy = handle_destroy_relative_pointer_manager,
        .get_relative_pointer = handle_get_relative_pointer_relative_pointer_manager,
};

static void
handle_bind_relative_pointer(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SERVER_RELATIVE_POINTER_VERSION);
    struct compositor *compositor = data;

    struct wl_resource *resource =
        wl_resource_create(client, &zwp_relative_pointer_manager_v1_interface,
                           zwp_relative_pointer_manager_v1_interface.version, id);
    wl_resource_set_implementation(resource, &relative_pointer_manager_implementation, compositor,
                                   destroy_relative_pointer_manager);
}

static void
destroy_dmabuf(struct dmabuf *dmabuf) {
    close(dmabuf->fd);
    wl_list_remove(&dmabuf->link);
    free(dmabuf);
}

static void
destroy_server_buffer(struct server_buffer *server_buffer) {
    if (server_buffer->buffer) {
        wl_buffer_destroy(server_buffer->buffer);
        server_buffer->buffer = NULL;
    }

    switch (server_buffer->type) {
    case BUFFER_SHM:
        // Buffers from wl_shm_pool don't need any cleanup, the pool is responsible for their
        // resources.
        break;
    case BUFFER_DMABUF:;
        struct dmabuf *dmabuf, *tmp;
        wl_list_for_each_safe (dmabuf, tmp, &server_buffer->data.dmabuf.bufs, link) {
            destroy_dmabuf(dmabuf);
        }
        break;
    }

    free(server_buffer);
}

static void
handle_destroy_wl_buffer(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
destroy_wl_buffer(struct wl_resource *resource) {
    struct server_buffer *server_buffer = wl_resource_get_user_data(resource);
    destroy_server_buffer(server_buffer);
}

static const struct wl_buffer_interface wl_buffer_implementation = {
    .destroy = handle_destroy_wl_buffer,
};

static void
handle_create_buffer_wl_shm_pool(struct wl_client *client, struct wl_resource *resource,
                                 uint32_t id, int32_t offset, int32_t width, int32_t height,
                                 int32_t stride, uint32_t format) {
    struct wl_shm_pool *shm_pool = wl_resource_get_user_data(resource);

    struct wl_buffer *wl_buffer =
        wl_shm_pool_create_buffer(shm_pool, offset, width, height, stride, format);
    struct server_buffer *server_buffer = ww_alloc(1, sizeof(*server_buffer));
    struct wl_resource *buffer_resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
    wl_resource_set_implementation(buffer_resource, &wl_buffer_implementation, server_buffer,
                                   destroy_wl_buffer);

    server_buffer->type = BUFFER_SHM;
    server_buffer->buffer = wl_buffer;
    server_buffer->data.shm.offset = offset;
    server_buffer->data.shm.width = width;
    server_buffer->data.shm.height = height;
    server_buffer->data.shm.stride = stride;
    server_buffer->data.shm.format = format;
}

static void
handle_destroy_wl_shm_pool(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_resize_wl_shm_pool(struct wl_client *client, struct wl_resource *resource, int32_t size) {
    struct wl_shm_pool *shm_pool = wl_resource_get_user_data(resource);

    wl_shm_pool_resize(shm_pool, size);
}

static void
destroy_wl_shm_pool(struct wl_resource *resource) {
    struct wl_shm_pool *shm_pool = wl_resource_get_user_data(resource);

    wl_shm_pool_destroy(shm_pool);
}

static const struct wl_shm_pool_interface wl_shm_pool_implementation = {
    .create_buffer = handle_create_buffer_wl_shm_pool,
    .destroy = handle_destroy_wl_shm_pool,
    .resize = handle_resize_wl_shm_pool,
};

static void
handle_create_pool_wl_shm(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                          int32_t fd, int32_t size) {
    struct compositor *compositor = wl_resource_get_user_data(resource);

    struct wl_shm_pool *shm_pool = wl_shm_create_pool(compositor->remote.shm, fd, size);
    close(fd);
    struct wl_resource *pool_resource =
        wl_resource_create(client, &wl_shm_pool_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(pool_resource, &wl_shm_pool_implementation, shm_pool,
                                   destroy_wl_shm_pool);
}

static void
destroy_wl_shm(struct wl_resource *resource) {
    // Unused.
}

static const struct wl_shm_interface wl_shm_implementation = {
    .create_pool = handle_create_pool_wl_shm,
};

static void
handle_bind_wl_shm(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SERVER_WL_SHM_VERSION);
    struct compositor *compositor = data;

    struct wl_resource *resource =
        wl_resource_create(client, &wl_shm_interface, wl_shm_interface.version, id);
    wl_resource_set_implementation(resource, &wl_shm_implementation, compositor, destroy_wl_shm);

    uint32_t *format;
    wl_array_for_each(format, &compositor->remote.shm_formats) {
        wl_shm_send_format(resource, *format);
    }
}

static void
on_buffer_release(void *data, struct wl_buffer *wl_buffer) {
    struct wl_resource *buffer_resource = data;

    wl_buffer_send_release(buffer_resource);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = on_buffer_release,
};

struct server_buffer_params {
    struct compositor *compositor;
    struct zwp_linux_buffer_params_v1 *buffer_params;

    struct server_buffer *buffer;
    struct wl_resource *buffer_resource;

    bool issued_create, failed;
};

static void
on_buffer_params_created(void *data, struct zwp_linux_buffer_params_v1 *buffer_params,
                         struct wl_buffer *wl_buffer) {
    struct wl_resource *buffer_params_resource = data;
    struct server_buffer_params *server_buffer_params =
        wl_resource_get_user_data(buffer_params_resource);

    server_buffer_params->buffer->buffer = wl_buffer;

    server_buffer_params->buffer_resource = wl_resource_create(
        wl_resource_get_client(buffer_params_resource), &wl_buffer_interface, 1, 0);
    wl_resource_set_implementation(server_buffer_params->buffer_resource, &wl_buffer_implementation,
                                   server_buffer_params->buffer, destroy_wl_buffer);

    wl_buffer_add_listener(wl_buffer, &buffer_listener, server_buffer_params->buffer_resource);

    zwp_linux_buffer_params_v1_send_created(buffer_params_resource,
                                            server_buffer_params->buffer_resource);
}

static void
on_buffer_params_failed(void *data, struct zwp_linux_buffer_params_v1 *buffer_params) {
    struct wl_resource *buffer_params_resource = data;
    struct server_buffer_params *server_buffer_params =
        wl_resource_get_user_data(buffer_params_resource);

    server_buffer_params->failed = true;
    zwp_linux_buffer_params_v1_send_failed(buffer_params_resource);
}

static const struct zwp_linux_buffer_params_v1_listener buffer_params_listener = {
    .created = on_buffer_params_created,
    .failed = on_buffer_params_failed,
};

static void
handle_destroy_linux_buffer_params(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_add_linux_buffer_params(struct wl_client *client, struct wl_resource *resource, int32_t fd,
                               uint32_t plane_idx, uint32_t offset, uint32_t stride,
                               uint32_t modifier_hi, uint32_t modifier_lo) {
    struct server_buffer_params *server_buffer_params = wl_resource_get_user_data(resource);

    // TODO: invalid format error if modifier+format are unsupported
    // TODO: plane_idx error
    // TODO: plane_set error

    struct dmabuf *dmabuf = ww_alloc(1, sizeof(*dmabuf));
    dmabuf->fd = fd;
    dmabuf->plane_idx = plane_idx;
    dmabuf->offset = offset;
    dmabuf->stride = stride;
    dmabuf->modifier = (((uint64_t)modifier_hi) << 32) | (uint64_t)modifier_lo;
    wl_list_insert(&server_buffer_params->buffer->data.dmabuf.bufs, &dmabuf->link);

    zwp_linux_buffer_params_v1_add(server_buffer_params->buffer_params, fd, plane_idx, offset,
                                   stride, modifier_hi, modifier_lo);
}

static void
handle_create_linux_buffer_params(struct wl_client *client, struct wl_resource *resource,
                                  int32_t width, int32_t height, uint32_t format, uint32_t flags) {
    struct server_buffer_params *server_buffer_params = wl_resource_get_user_data(resource);
    struct compositor *compositor = server_buffer_params->compositor;

    if (server_buffer_params->issued_create) {
        wl_resource_post_error(
            resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
            "create or create_immed already called on zwp_linux_buffer_params_v1 object");
        return;
    }

    // TODO: errors (incomplete, invalid_format, invalid_dimensions, out_of_bounds)

    server_buffer_params->buffer->data.dmabuf.width = width;
    server_buffer_params->buffer->data.dmabuf.height = height;
    server_buffer_params->buffer->data.dmabuf.format = format;
    server_buffer_params->buffer->data.dmabuf.flags = flags;

    server_buffer_params->issued_create = true;

    // Roundtrip to receive the `created` or `failed` event from the host compositor.
    zwp_linux_buffer_params_v1_create(server_buffer_params->buffer_params, width, height, format,
                                      flags);
    wl_display_roundtrip(compositor->remote.display);
}

static void
handle_create_immed_linux_buffer_params(struct wl_client *client, struct wl_resource *resource,
                                        uint32_t id, int32_t width, int32_t height, uint32_t format,
                                        uint32_t flags) {
    struct server_buffer_params *server_buffer_params = wl_resource_get_user_data(resource);
    struct compositor *compositor = server_buffer_params->compositor;

    if (server_buffer_params->issued_create) {
        wl_resource_post_error(
            resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
            "create or create_immed already called on zwp_linux_buffer_params_v1 object");
        return;
    }

    // TODO: errors (incomplete, invalid_format, invalid_dimensions, out_of_bounds)

    server_buffer_params->buffer->data.dmabuf.width = width;
    server_buffer_params->buffer->data.dmabuf.height = height;
    server_buffer_params->buffer->data.dmabuf.format = format;
    server_buffer_params->buffer->data.dmabuf.flags = flags;

    server_buffer_params->issued_create = true;

    // Roundtrip in case we receive a `failed` event from the host compositor.
    server_buffer_params->buffer->buffer = zwp_linux_buffer_params_v1_create_immed(
        server_buffer_params->buffer_params, width, height, format, flags);
    wl_display_roundtrip(compositor->remote.display);

    if (server_buffer_params->failed) {
        server_buffer_params->buffer->buffer = NULL;
        destroy_server_buffer(server_buffer_params->buffer);
    }

    server_buffer_params->buffer_resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
    wl_resource_set_implementation(
        server_buffer_params->buffer_resource, &wl_buffer_implementation,
        server_buffer_params->failed ? NULL : server_buffer_params->buffer, destroy_wl_buffer);

    wl_buffer_add_listener(server_buffer_params->buffer->buffer, &buffer_listener,
                           server_buffer_params->buffer_resource);
}

static void
destroy_linux_buffer_params(struct wl_resource *resource) {
    struct server_buffer_params *server_buffer_params = wl_resource_get_user_data(resource);

    if (server_buffer_params->buffer_params) {
        zwp_linux_buffer_params_v1_destroy(server_buffer_params->buffer_params);
    }

    wl_list_remove(wl_resource_get_link(resource));
    free(server_buffer_params);
}

static const struct zwp_linux_buffer_params_v1_interface linux_buffer_params_implementation = {
    .destroy = handle_destroy_linux_buffer_params,
    .add = handle_add_linux_buffer_params,
    .create = handle_create_linux_buffer_params,
    .create_immed = handle_create_immed_linux_buffer_params,
};

static void
on_dmabuf_feedback_done(void *data, struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback) {
    struct wl_resource *feedback_resource = data;

    zwp_linux_dmabuf_feedback_v1_send_done(feedback_resource);
}

static void
on_dmabuf_feedback_format_table(void *data, struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
                                int32_t fd, uint32_t size) {
    struct wl_resource *feedback_resource = data;

    zwp_linux_dmabuf_feedback_v1_send_format_table(feedback_resource, fd, size);
    close(fd);
}

static void
on_dmabuf_feedback_main_device(void *data, struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
                               struct wl_array *device) {
    struct wl_resource *feedback_resource = data;

    zwp_linux_dmabuf_feedback_v1_send_main_device(feedback_resource, device);
}

static void
on_dmabuf_feedback_tranche_done(void *data, struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback) {
    struct wl_resource *feedback_resource = data;

    zwp_linux_dmabuf_feedback_v1_send_tranche_done(feedback_resource);
}

static void
on_dmabuf_feedback_tranche_target_device(void *data,
                                         struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
                                         struct wl_array *device) {
    struct wl_resource *feedback_resource = data;

    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(feedback_resource, device);
}

static void
on_dmabuf_feedback_tranche_formats(void *data, struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
                                   struct wl_array *indices) {
    struct wl_resource *feedback_resource = data;

    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(feedback_resource, indices);
}

static void
on_dmabuf_feedback_tranche_flags(void *data, struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback,
                                 uint32_t flags) {
    struct wl_resource *feedback_resource = data;

    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(feedback_resource, flags);
}

static const struct zwp_linux_dmabuf_feedback_v1_listener dmabuf_feedback_listener = {
    .done = on_dmabuf_feedback_done,
    .format_table = on_dmabuf_feedback_format_table,
    .main_device = on_dmabuf_feedback_main_device,
    .tranche_done = on_dmabuf_feedback_tranche_done,
    .tranche_target_device = on_dmabuf_feedback_tranche_target_device,
    .tranche_formats = on_dmabuf_feedback_tranche_formats,
    .tranche_flags = on_dmabuf_feedback_tranche_flags,
};

static void
handle_destroy_linux_dmabuf_feedback(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
destroy_linux_dmabuf_feedback(struct wl_resource *resource) {
    struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback = wl_resource_get_user_data(resource);

    zwp_linux_dmabuf_feedback_v1_destroy(dmabuf_feedback);
    wl_list_remove(wl_resource_get_link(resource));
}

static const struct zwp_linux_dmabuf_feedback_v1_interface linux_dmabuf_feedback_implementation = {
    .destroy = handle_destroy_linux_dmabuf_feedback,
};

struct linux_dmabuf {
    struct compositor *compositor;

    struct wl_list buffer_params;
    struct wl_list dmabuf_feedback;
};

static void
handle_destroy_linux_dmabuf(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_create_params_linux_dmabuf(struct wl_client *client, struct wl_resource *resource,
                                  uint32_t id) {
    struct linux_dmabuf *linux_dmabuf = wl_resource_get_user_data(resource);
    struct compositor *compositor = linux_dmabuf->compositor;

    struct server_buffer_params *server_buffer_params = ww_alloc(1, sizeof(*server_buffer_params));
    struct wl_resource *buffer_params_resource = wl_resource_create(
        client, &zwp_linux_buffer_params_v1_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(buffer_params_resource, &linux_buffer_params_implementation,
                                   server_buffer_params, destroy_linux_buffer_params);

    server_buffer_params->buffer = ww_alloc(1, sizeof(*server_buffer_params->buffer));
    server_buffer_params->compositor = compositor;
    server_buffer_params->buffer_params =
        zwp_linux_dmabuf_v1_create_params(compositor->remote.linux_dmabuf);
    zwp_linux_buffer_params_v1_add_listener(server_buffer_params->buffer_params,
                                            &buffer_params_listener, buffer_params_resource);

    server_buffer_params->buffer->type = BUFFER_DMABUF;
    wl_list_init(&server_buffer_params->buffer->data.dmabuf.bufs);

    wl_list_insert(&linux_dmabuf->buffer_params, wl_resource_get_link(buffer_params_resource));

    // Roundtrip to pass the initial events from the host compositor to the client.
    wl_display_roundtrip(compositor->remote.display);
}

static void
handle_get_default_feedback_linux_dmabuf(struct wl_client *client, struct wl_resource *resource,
                                         uint32_t id) {
    struct linux_dmabuf *linux_dmabuf = wl_resource_get_user_data(resource);
    struct compositor *compositor = linux_dmabuf->compositor;

    struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback =
        zwp_linux_dmabuf_v1_get_default_feedback(compositor->remote.linux_dmabuf);
    struct wl_resource *feedback_resource = wl_resource_create(
        client, &zwp_linux_dmabuf_feedback_v1_interface, wl_resource_get_version(resource), id);
    zwp_linux_dmabuf_feedback_v1_add_listener(dmabuf_feedback, &dmabuf_feedback_listener,
                                              feedback_resource);
    wl_resource_set_implementation(feedback_resource, &linux_dmabuf_feedback_implementation,
                                   dmabuf_feedback, destroy_linux_dmabuf_feedback);

    wl_list_insert(&linux_dmabuf->dmabuf_feedback, wl_resource_get_link(feedback_resource));

    // Roundtrip to pass the initial events from the host compositor to the client.
    wl_display_roundtrip(compositor->remote.display);
}

static void
handle_get_surface_feedback_linux_dmabuf(struct wl_client *client, struct wl_resource *resource,
                                         uint32_t id, struct wl_resource *surface_resource) {
    struct linux_dmabuf *linux_dmabuf = wl_resource_get_user_data(resource);
    struct server_surface *server_surface = wl_resource_get_user_data(surface_resource);
    struct compositor *compositor = linux_dmabuf->compositor;

    struct zwp_linux_dmabuf_feedback_v1 *dmabuf_feedback = zwp_linux_dmabuf_v1_get_surface_feedback(
        compositor->remote.linux_dmabuf, server_surface->surface);
    struct wl_resource *feedback_resource = wl_resource_create(
        client, &zwp_linux_dmabuf_feedback_v1_interface, wl_resource_get_version(resource), id);
    zwp_linux_dmabuf_feedback_v1_add_listener(dmabuf_feedback, &dmabuf_feedback_listener,
                                              feedback_resource);
    wl_resource_set_implementation(feedback_resource, &linux_dmabuf_feedback_implementation,
                                   dmabuf_feedback, destroy_linux_dmabuf_feedback);

    wl_list_insert(&linux_dmabuf->dmabuf_feedback, wl_resource_get_link(feedback_resource));

    // Roundtrip to pass the initial events from the host compositor to the client.
    wl_display_roundtrip(compositor->remote.display);
}

static void
destroy_linux_dmabuf(struct wl_resource *resource) {
    struct linux_dmabuf *linux_dmabuf = wl_resource_get_user_data(resource);

    struct wl_resource *buffer_params_resource, *tmp;
    wl_resource_for_each_safe(buffer_params_resource, tmp, &linux_dmabuf->buffer_params) {
        wl_resource_destroy(buffer_params_resource);
    }

    struct wl_resource *dmabuf_feedback_resource;
    wl_resource_for_each_safe(dmabuf_feedback_resource, tmp, &linux_dmabuf->dmabuf_feedback) {
        wl_resource_destroy(dmabuf_feedback_resource);
    }

    free(linux_dmabuf);
}

static const struct zwp_linux_dmabuf_v1_interface linux_dmabuf_implementation = {
    .destroy = handle_destroy_linux_dmabuf,
    .create_params = handle_create_params_linux_dmabuf,
    .get_default_feedback = handle_get_default_feedback_linux_dmabuf,
    .get_surface_feedback = handle_get_surface_feedback_linux_dmabuf,
};

static void
handle_bind_linux_dmabuf(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SERVER_LINUX_DMABUF_VERSION);

    if (version < 4) {
        wl_client_post_implementation_error(
            client, "wp_linux_dmabuf versions older than 4 are unsupported");
        return;
    }

    struct compositor *compositor = data;

    struct linux_dmabuf *linux_dmabuf = ww_alloc(1, sizeof(*linux_dmabuf));
    struct wl_resource *resource = wl_resource_create(client, &zwp_linux_dmabuf_v1_interface,
                                                      zwp_linux_dmabuf_v1_interface.version, id);
    wl_resource_set_implementation(resource, &linux_dmabuf_implementation, linux_dmabuf,
                                   destroy_linux_dmabuf);

    linux_dmabuf->compositor = compositor;
    wl_list_init(&linux_dmabuf->buffer_params);
    wl_list_init(&linux_dmabuf->dmabuf_feedback);
}

static void
handle_release_wl_output(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
destroy_wl_output(struct wl_resource *resource) {
    // Unused.
}

static const struct wl_output_interface wl_output_implementation = {.release =
                                                                        handle_release_wl_output};

static void
handle_bind_wl_output(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SERVER_WL_OUTPUT_VERSION);
    struct compositor *compositor = data;

    struct wl_resource *resource =
        wl_resource_create(client, &wl_output_interface, wl_output_interface.version, id);
    wl_resource_set_implementation(resource, &wl_output_implementation, compositor,
                                   destroy_wl_output);

    if (version >= WL_OUTPUT_SCALE_SINCE_VERSION) {
        wl_output_send_scale(resource, 1);
    }

    // GLFW does not make use of subpixel or transformation information as of v3.3.8.
    wl_output_send_geometry(resource, 0, 0, 0, 0, WL_OUTPUT_SUBPIXEL_NONE, "Waywall", "Waywall",
                            WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT, compositor->remote.win_width,
                        compositor->remote.win_height, 0);
    if (version >= WL_OUTPUT_NAME_SINCE_VERSION) {
        wl_output_send_name(resource, "WAYWALL");
    }
    if (version >= WL_OUTPUT_DONE_SINCE_VERSION) {
        wl_output_send_done(resource);
    }
}

static void
handle_release_wl_pointer(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_set_cursor_wl_pointer(struct wl_client *client, struct wl_resource *resource,
                             uint32_t serial, struct wl_resource *surface_resource,
                             int32_t hotspot_x, int32_t hotspot_y) {
    struct server_pointer *server_pointer = wl_resource_get_user_data(resource);
    if (!server_pointer->server_seat) {
        // This pointer object has been invalidated (the pointer capability was removed.)
        return;
    }

    // We don't want to do what the client asks because it's wrong. GLFW doesn't hide the cursor
    // image in some cases where it should. Instead, we just give the surface a role.
    struct server_surface *server_surface = wl_resource_get_user_data(surface_resource);
    if (server_surface->role == ROLE_XDG) {
        wl_resource_post_error(resource, WL_POINTER_ERROR_ROLE,
                               "cursor surface already has xdg_toplevel role");
        return;
    }
    server_surface->role = ROLE_CURSOR;
}

static void
destroy_wl_pointer(struct wl_resource *resource) {
    struct server_pointer *server_pointer = wl_resource_get_user_data(resource);

    if (!server_pointer->server_seat) {
        // The pointer capability was not removed (this wl_pointer object is still valid) and so
        // it must be removed from the list.
        wl_list_remove(wl_resource_get_link(resource));
    }

    struct wl_resource *relative_pointer_resource, *tmp;
    wl_resource_for_each_safe(relative_pointer_resource, tmp, &server_pointer->relative_pointers) {
        wl_resource_set_user_data(relative_pointer_resource, NULL);
        wl_list_remove(wl_resource_get_link(relative_pointer_resource));
    }

    free(server_pointer);
}

static const struct wl_pointer_interface wl_pointer_implementation = {
    .release = handle_release_wl_pointer,
    .set_cursor = handle_set_cursor_wl_pointer,
};

static void
handle_release_wl_keyboard(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
destroy_wl_keyboard(struct wl_resource *resource) {
    struct server_seat *server_seat = wl_resource_get_user_data(resource);

    if (server_seat) {
        // The keyboard capability was not removed (this wl_keyboard object is still valid) and so
        // it must be removed from the list.
        wl_list_remove(wl_resource_get_link(resource));
    }
}

static const struct wl_keyboard_interface wl_keyboard_implementation = {
    .release = handle_release_wl_keyboard,
};

static void
handle_get_pointer_wl_seat(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_seat *server_seat = wl_resource_get_user_data(resource);

    if ((server_seat->past_caps & WL_SEAT_CAPABILITY_POINTER) == 0) {
        wl_resource_post_error(
            resource, WL_SEAT_ERROR_MISSING_CAPABILITY,
            "wl_seat.get_pointer called when no pointer capability was ever advertised");
        return;
    }

    struct server_pointer *server_pointer = ww_alloc(1, sizeof(*server_pointer));
    struct wl_resource *pointer_resource =
        wl_resource_create(client, &wl_pointer_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(pointer_resource, &wl_pointer_implementation, server_pointer,
                                   destroy_wl_pointer);
    wl_list_insert(&server_seat->pointers, wl_resource_get_link(pointer_resource));

    server_pointer->server_seat = NULL;
    wl_list_init(&server_pointer->relative_pointers);
}

static void
handle_get_keyboard_wl_seat(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct server_seat *server_seat = wl_resource_get_user_data(resource);

    if ((server_seat->past_caps & WL_SEAT_CAPABILITY_KEYBOARD) == 0) {
        wl_resource_post_error(
            resource, WL_SEAT_ERROR_MISSING_CAPABILITY,
            "wl_seat.get_keyboard called when no keyboard capability was ever advertised");
        return;
    }

    struct wl_resource *keyboard_resource =
        wl_resource_create(client, &wl_keyboard_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(keyboard_resource, &wl_keyboard_implementation, server_seat,
                                   destroy_wl_keyboard);
    wl_list_insert(&server_seat->keyboards, wl_resource_get_link(keyboard_resource));

    // TODO: send repeat_info
    // TODO: send keymap
}

static void
handle_get_touch_wl_seat(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    wl_resource_post_error(
        resource, WL_SEAT_ERROR_MISSING_CAPABILITY,
        "wl_seat.get_touch was called when no touch capability was ever advertised");
}

static void
handle_release_wl_seat(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
destroy_wl_seat(struct wl_resource *resource) {
    struct server_seat *server_seat = wl_resource_get_user_data(resource);

    struct wl_resource *pointer_resource, *tmp;
    wl_resource_for_each_safe(pointer_resource, tmp, &server_seat->pointers) {
        wl_resource_destroy(pointer_resource);
    }

    struct wl_resource *keyboard_resource;
    wl_resource_for_each_safe(keyboard_resource, tmp, &server_seat->keyboards) {
        wl_resource_destroy(keyboard_resource);
    }

    wl_list_remove(wl_resource_get_link(resource));
    free(server_seat);
}

static const struct wl_seat_interface wl_seat_implementation = {
    .get_pointer = handle_get_pointer_wl_seat,
    .get_keyboard = handle_get_keyboard_wl_seat,
    .get_touch = handle_get_touch_wl_seat,
    .release = handle_release_wl_seat,
};

static void
handle_bind_wl_seat(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SERVER_WL_SEAT_VERSION);
    struct client_seat *client_seat = data;

    struct server_seat *server_seat = ww_alloc(1, sizeof(*server_seat));
    struct wl_resource *resource =
        wl_resource_create(client, &wl_seat_interface, wl_seat_interface.version, id);
    wl_resource_set_implementation(resource, &wl_seat_implementation, server_seat, destroy_wl_seat);

    server_seat->client_seat = client_seat;
    server_seat->version = version;
    wl_list_init(&server_seat->pointers);
    wl_list_init(&server_seat->keyboards);

    wl_list_insert(&client_seat->clients, wl_resource_get_link(resource));

    if (version >= WL_SEAT_NAME_SINCE_VERSION) {
        wl_seat_send_name(resource, client_seat->str_name);
    }

    server_seat->caps = client_seat->caps;
    server_seat->past_caps = client_seat->caps;

    wl_seat_send_capabilities(resource, server_seat->caps);
}

static void
server_seat_handle_caps(struct server_seat *server_seat, uint32_t caps) {
    // Invalidate any existing wl_pointer objects that belong to this seat.
    if ((caps & WL_SEAT_CAPABILITY_POINTER) == 0) {
        struct wl_resource *pointer_resource, *tmp;
        wl_resource_for_each_safe(pointer_resource, tmp, &server_seat->pointers) {
            struct server_pointer *server_pointer = wl_resource_get_user_data(pointer_resource);
            server_pointer->server_seat = NULL;
            wl_list_remove(wl_resource_get_link(pointer_resource));
        }
    }

    // Invalidate any existing wl_keyboard objects that belong to this seat.
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) == 0) {
        struct wl_resource *keyboard_resource, *tmp;
        wl_resource_for_each_safe(keyboard_resource, tmp, &server_seat->keyboards) {
            wl_resource_set_user_data(keyboard_resource, NULL);
            wl_list_remove(wl_resource_get_link(keyboard_resource));
        }
    }

    wl_seat_send_capabilities(server_seat->resource, caps);
}

static void
handle_destroy_xdg_decoration(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_set_mode_xdg_decoration(struct wl_client *client, struct wl_resource *resource,
                               uint32_t mode) {
    if (mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE) {
        wl_client_post_implementation_error(client, "requested client side decorations");
    }
}

static void
handle_unset_mode_xdg_decoration(struct wl_client *client, struct wl_resource *resource) {
    // Do nothing.
}

static void
destroy_xdg_decoration(struct wl_resource *resource) {
    struct server_xdg_toplevel *server_xdg_toplevel = wl_resource_get_user_data(resource);

    server_xdg_toplevel->decoration_resource = NULL;
}

static const struct zxdg_toplevel_decoration_v1_interface xdg_decoration_implementation = {
    .destroy = handle_destroy_xdg_decoration,
    .set_mode = handle_set_mode_xdg_decoration,
    .unset_mode = handle_unset_mode_xdg_decoration,
};

static void
handle_destroy_xdg_decoration_manager(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_get_toplevel_decoration_xdg_decoration_manager(struct wl_client *client,
                                                      struct wl_resource *resource, uint32_t id,
                                                      struct wl_resource *toplevel_resource) {
    struct server_xdg_toplevel *server_xdg_toplevel = wl_resource_get_user_data(toplevel_resource);

    if (server_xdg_toplevel->decoration_resource) {
        wl_resource_post_error(resource, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED,
                               "xdg_toplevel already had an xdg_toplevel_decoration instance");
        return;
    }
    if (server_xdg_toplevel->server_xdg_surface->server_surface->buffer_resource) {
        // TODO: we do not error in wl_surface.attach if xdg_toplevel_decoration.configure has not
        // yet been received

        wl_resource_post_error(
            resource, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_UNCONFIGURED_BUFFER,
            "xdg_surface already had attached buffer when creating xdg_toplevel_decoration");
        return;
    }

    struct wl_resource *xdg_decoration_resource = wl_resource_create(
        client, &zxdg_toplevel_decoration_v1_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(xdg_decoration_resource, &xdg_decoration_implementation,
                                   server_xdg_toplevel, destroy_xdg_decoration);

    server_xdg_toplevel->decoration_resource = xdg_decoration_resource;

    zxdg_toplevel_decoration_v1_send_configure(xdg_decoration_resource,
                                               ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void
destroy_xdg_decoration_manager(struct wl_resource *resource) {
    // Unused.
}

static const struct zxdg_decoration_manager_v1_interface xdg_decoration_manager_implementation = {
    .destroy = handle_destroy_xdg_decoration_manager,
    .get_toplevel_decoration = handle_get_toplevel_decoration_xdg_decoration_manager,
};

static void
handle_bind_xdg_decoration(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SERVER_XDG_DECORATION_VERSION);
    struct compositor *compositor = data;

    struct wl_resource *resource =
        wl_resource_create(client, &zxdg_decoration_manager_v1_interface,
                           zxdg_decoration_manager_v1_interface.version, id);
    wl_resource_set_implementation(resource, &xdg_decoration_manager_implementation, compositor,
                                   destroy_xdg_decoration_manager);
}

static void
handle_destroy_xdg_toplevel(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_set_parent_xdg_toplevel(struct wl_client *client, struct wl_resource *resource,
                               struct wl_resource *parent_resource) {
    // GLFW doesn't use xdg_toplevel.set_parent.
    wl_client_post_implementation_error(client, "xdg_toplevel.set_parent is not implemented");
}

static void
handle_set_title_xdg_toplevel(struct wl_client *client, struct wl_resource *resource,
                              const char *title) {
    // Do nothing. We don't care about window titles.
}

static void
handle_set_app_id_xdg_toplevel(struct wl_client *client, struct wl_resource *resource,
                               const char *app_id) {
    // Do nothing. We don't care about app IDs.
}

static void
handle_show_window_menu_xdg_toplevel(struct wl_client *client, struct wl_resource *resource,
                                     struct wl_resource *seat_resource, uint32_t serial, int32_t x,
                                     int32_t y) {
    // Do nothing. We don't care about window menus.
}

static void
handle_move_xdg_toplevel(struct wl_client *client, struct wl_resource *resource,
                         struct wl_resource *seat_resource, uint32_t serial) {
    // Do nothing. We don't care about interactive moves.
}

static void
handle_resize_xdg_toplevel(struct wl_client *client, struct wl_resource *resource,
                           struct wl_resource *seat_resource, uint32_t serial, uint32_t edges) {
    // Do nothing. We don't care about interactive resizes.
}

static void
handle_set_max_size_xdg_toplevel(struct wl_client *client, struct wl_resource *resource,
                                 int32_t width, int32_t height) {
    struct server_xdg_toplevel *server_xdg_toplevel = wl_resource_get_user_data(resource);

    // We don't care about maximum size, but we'll check that the request is valid.
    if (width < 0 || height < 0) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_SIZE,
                               "negative max size provided");
        return;
    }
    if (width > 0 && width < server_xdg_toplevel->min_width) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_SIZE,
                               "max width smaller than minimum width provided");
        return;
    }
    if (height > 0 && height < server_xdg_toplevel->min_height) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_SIZE,
                               "max height smaller than minimum height provided");
        return;
    }

    server_xdg_toplevel->max_width = width;
    server_xdg_toplevel->max_height = height;
}

static void
handle_set_min_size_xdg_toplevel(struct wl_client *client, struct wl_resource *resource,
                                 int32_t width, int32_t height) {
    struct server_xdg_toplevel *server_xdg_toplevel = wl_resource_get_user_data(resource);

    // We don't care about minimum size, but we'll check that the request is valid.
    if (width < 0 || height < 0) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_SIZE,
                               "negative minimum size provided");
        return;
    }
    if (server_xdg_toplevel->max_width > 0 && width > server_xdg_toplevel->max_width) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_SIZE,
                               "minimum width larger than max width provided");
        return;
    }
    if (server_xdg_toplevel->max_height > 0 && height > server_xdg_toplevel->max_height) {
        wl_resource_post_error(resource, XDG_TOPLEVEL_ERROR_INVALID_SIZE,
                               "minimum height larger than max height provided");
        return;
    }

    server_xdg_toplevel->min_width = width;
    server_xdg_toplevel->min_height = height;
}

static void
handle_set_maximized_xdg_toplevel(struct wl_client *client, struct wl_resource *resource) {
    struct server_xdg_toplevel *server_xdg_toplevel = wl_resource_get_user_data(resource);

    if (server_xdg_toplevel->server_xdg_surface->fullscreen) {
        return;
    }

    // We don't care about maximization.
    send_xdg_toplevel_configure(resource);
    send_xdg_surface_configure(
        server_xdg_toplevel->server_xdg_surface->server_surface->xdg_resource);
}

static void
handle_unset_maximized_xdg_toplevel(struct wl_client *client, struct wl_resource *resource) {
    struct server_xdg_toplevel *server_xdg_toplevel = wl_resource_get_user_data(resource);

    if (server_xdg_toplevel->server_xdg_surface->fullscreen) {
        return;
    }

    // We don't care about maximization.
    send_xdg_toplevel_configure(resource);
    send_xdg_surface_configure(
        server_xdg_toplevel->server_xdg_surface->server_surface->xdg_resource);
}

static void
handle_set_fullscreen_xdg_toplevel(struct wl_client *client, struct wl_resource *resource,
                                   struct wl_resource *output_resource) {
    struct server_xdg_toplevel *server_xdg_toplevel = wl_resource_get_user_data(resource);

    // TODO: do not allow fullscreen toggle on wall

    server_xdg_toplevel->server_xdg_surface->fullscreen = true;

    send_xdg_toplevel_configure(resource);
    send_xdg_surface_configure(
        server_xdg_toplevel->server_xdg_surface->server_surface->xdg_resource);
}

static void
handle_unset_fullscreen_xdg_toplevel(struct wl_client *client, struct wl_resource *resource) {
    struct server_xdg_toplevel *server_xdg_toplevel = wl_resource_get_user_data(resource);

    server_xdg_toplevel->server_xdg_surface->fullscreen = false;

    send_xdg_toplevel_configure(resource);
    send_xdg_surface_configure(
        server_xdg_toplevel->server_xdg_surface->server_surface->xdg_resource);
}

static void
handle_set_minimized_xdg_toplevel(struct wl_client *client, struct wl_resource *resource) {
    // Do nothing.
}

static void
destroy_xdg_toplevel(struct wl_resource *resource) {
    struct server_xdg_toplevel *server_xdg_toplevel = wl_resource_get_user_data(resource);

    server_xdg_toplevel->server_xdg_surface->toplevel_resource = NULL;
    server_xdg_toplevel->server_xdg_surface->server_surface->role = ROLE_NONE;
    if (server_xdg_toplevel->decoration_resource) {
        wl_resource_post_error(server_xdg_toplevel->decoration_resource,
                               ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ORPHANED,
                               "xdg_toplevel destroyed before associated decoration");
        wl_resource_destroy(server_xdg_toplevel->decoration_resource);
    }
    if (server_xdg_toplevel->server_xdg_surface->server_surface->subsurface) {
        wl_subsurface_destroy(server_xdg_toplevel->server_xdg_surface->server_surface->subsurface);
    }

    free(server_xdg_toplevel);
}

static const struct xdg_toplevel_interface xdg_toplevel_implementation = {
    .destroy = handle_destroy_xdg_toplevel,
    .set_parent = handle_set_parent_xdg_toplevel,
    .set_title = handle_set_title_xdg_toplevel,
    .set_app_id = handle_set_app_id_xdg_toplevel,
    .show_window_menu = handle_show_window_menu_xdg_toplevel,
    .move = handle_move_xdg_toplevel,
    .resize = handle_resize_xdg_toplevel,
    .set_max_size = handle_set_max_size_xdg_toplevel,
    .set_min_size = handle_set_min_size_xdg_toplevel,
    .set_maximized = handle_set_maximized_xdg_toplevel,
    .unset_maximized = handle_unset_maximized_xdg_toplevel,
    .set_fullscreen = handle_set_fullscreen_xdg_toplevel,
    .unset_fullscreen = handle_unset_fullscreen_xdg_toplevel,
    .set_minimized = handle_set_minimized_xdg_toplevel,
};

static void
send_xdg_surface_configure(struct wl_resource *xdg_surface_resource) {
    struct server_xdg_surface *server_xdg_surface = wl_resource_get_user_data(xdg_surface_resource);
    struct ringbuf *serials = &server_xdg_surface->configure_serials;

    if (serials->count == RINGBUF_SIZE) {
        wl_client_post_implementation_error(wl_resource_get_client(xdg_surface_resource),
                                            "too many queued configure events");
        return;
    }

    uint32_t next = next_serial(xdg_surface_resource);
    bool ok = ringbuf_push(serials, next);
    ww_assert(ok);

    xdg_surface_send_configure(xdg_surface_resource, next);
}

static void
send_xdg_toplevel_configure(struct wl_resource *xdg_toplevel_resource) {
    struct server_xdg_toplevel *server_xdg_toplevel =
        wl_resource_get_user_data(xdg_toplevel_resource);
    struct server_xdg_surface *server_xdg_surface = server_xdg_toplevel->server_xdg_surface;

    struct wl_array states;
    wl_array_init(&states);
    if (server_xdg_surface->fullscreen) {
        uint32_t *state = wl_array_add(&states, sizeof(uint32_t));
        *state = XDG_TOPLEVEL_STATE_FULLSCREEN;
    }
    xdg_toplevel_send_configure(xdg_toplevel_resource, server_xdg_surface->width,
                                server_xdg_surface->height, &states);
    wl_array_release(&states);
}

static void
handle_destroy_xdg_surface(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_get_toplevel_xdg_surface(struct wl_client *client, struct wl_resource *resource,
                                uint32_t id) {
    struct server_xdg_surface *server_xdg_surface = wl_resource_get_user_data(resource);

    struct server_xdg_toplevel *server_xdg_toplevel = ww_alloc(1, sizeof(*server_xdg_toplevel));
    server_xdg_surface->toplevel_resource =
        wl_resource_create(client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(server_xdg_surface->toplevel_resource,
                                   &xdg_toplevel_implementation, server_xdg_toplevel,
                                   destroy_xdg_toplevel);

    server_xdg_surface->width = WINDOW_WIDTH;
    server_xdg_surface->height = WINDOW_HEIGHT;
    server_xdg_surface->fullscreen = false;

    server_xdg_toplevel->server_xdg_surface = server_xdg_surface;
    server_xdg_surface->server_surface->role = ROLE_XDG;
    server_xdg_surface->server_surface->subsurface = wl_subcompositor_get_subsurface(
        server_xdg_surface->server_surface->compositor->remote.subcompositor,
        server_xdg_surface->server_surface->surface,
        server_xdg_surface->server_surface->compositor->remote.surface);
    wl_subsurface_set_desync(server_xdg_surface->server_surface->subsurface);
    wl_surface_commit(server_xdg_surface->server_surface->compositor->remote.surface);
}

static void
handle_get_popup_xdg_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                             struct wl_resource *parent_resource,
                             struct wl_resource *positioner_resource) {
    // GLFW does not make use of xdg_popup so we will not implement it.
    wl_client_post_implementation_error(client, "xdg_surface.get_popup is not implemented");
}

static void
handle_set_window_geometry_xdg_surface(struct wl_client *client, struct wl_resource *resource,
                                       int32_t x, int32_t y, int32_t width, int32_t height) {
    // GLFW does not use xdg_surface.set_window_geometry, so we will not implement it.
    wl_client_post_implementation_error(client,
                                        "xdg_surface.set_window_geometry is not implemented");
}

static void
handle_ack_configure_xdg_surface(struct wl_client *client, struct wl_resource *resource,
                                 uint32_t serial) {
    struct server_xdg_surface *server_xdg_surface = wl_resource_get_user_data(resource);
    struct ringbuf *serials = &server_xdg_surface->configure_serials;

    for (size_t i = 0; i < serials->count; i++) {
        if (ringbuf_at(serials, i) == serial) {
            ringbuf_pop_n(serials, i + 1);
            server_xdg_surface->acked = true;
            return;
        }
    }

    wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SERIAL, "invalid serial %" PRIu32,
                           serial);
    return;
}

static void
destroy_xdg_surface(struct wl_resource *resource) {
    struct server_xdg_surface *server_xdg_surface = wl_resource_get_user_data(resource);

    if (server_xdg_surface->toplevel_resource) {
        wl_resource_post_error(resource, XDG_SURFACE_ERROR_DEFUNCT_ROLE_OBJECT,
                               "xdg_toplevel still exists for destroyed xdg_surface");
        wl_resource_destroy(server_xdg_surface->toplevel_resource);
    }

    server_xdg_surface->server_surface->xdg_resource = NULL;

    wl_list_remove(wl_resource_get_link(resource));
    free(server_xdg_surface);
}

static const struct xdg_surface_interface xdg_surface_implementation = {
    .destroy = handle_destroy_xdg_surface,
    .get_toplevel = handle_get_toplevel_xdg_surface,
    .get_popup = handle_get_popup_xdg_surface,
    .set_window_geometry = handle_set_window_geometry_xdg_surface,
    .ack_configure = handle_ack_configure_xdg_surface,
};

struct server_xdg_wm_base {
    struct compositor *compositor;

    struct wl_list surfaces;
};

static void
handle_destroy_xdg_wm_base(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void
handle_create_positioner_xdg_wm_base(struct wl_client *client, struct wl_resource *resource,
                                     uint32_t id) {
    // GLFW does not use xdg_positioner. Don't bother implementing it.
    wl_client_post_implementation_error(client, "xdg_positioner is not implemented");
}

static void
handle_get_xdg_surface_xdg_wm_base(struct wl_client *client, struct wl_resource *resource,
                                   uint32_t id, struct wl_resource *surface_resource) {
    struct server_xdg_wm_base *server_xdg_wm_base = wl_resource_get_user_data(resource);
    struct server_surface *server_surface = wl_resource_get_user_data(surface_resource);

    if (server_surface->role == ROLE_CURSOR) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE,
                               "cannot create xdg_surface from a surface with cursor role");
        return;
    }
    if (server_surface->xdg_resource) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE,
                               "cannot create multiple xdg_surfaces for one wl_surface");
        return;
    }

    struct server_xdg_surface *server_xdg_surface = ww_alloc(1, sizeof(*server_xdg_surface));
    server_surface->xdg_resource =
        wl_resource_create(client, &xdg_surface_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(server_surface->xdg_resource, &xdg_surface_implementation,
                                   server_xdg_surface, destroy_xdg_surface);

    server_xdg_surface->server_surface = server_surface;
    server_xdg_surface->client = client;

    wl_list_insert(&server_xdg_wm_base->surfaces,
                   wl_resource_get_link(server_surface->xdg_resource));
}

static void
handle_pong_xdg_wm_base(struct wl_client *client, struct wl_resource *resource, uint32_t serial) {
    // We don't care if clients are unresponsive and do not send the ping event.
    // TODO: Maybe we care later. Not for now though.
    wl_client_post_implementation_error(client, "received xdg_wm_base.pong with no ping event");
}

static void
destroy_xdg_wm_base(struct wl_resource *resource) {
    struct server_xdg_wm_base *server_xdg_wm_base = wl_resource_get_user_data(resource);

    int len = wl_list_length(&server_xdg_wm_base->surfaces);
    if (len > 0) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_DEFUNCT_SURFACES,
                               "%d surfaces still exist for destroyed xdg_wm_base", len);

        struct wl_resource *xdg_surface_resource, *tmp;
        wl_resource_for_each_safe(xdg_surface_resource, tmp, &server_xdg_wm_base->surfaces) {
            wl_resource_destroy(xdg_surface_resource);
        }
    }

    free(server_xdg_wm_base);
}

static const struct xdg_wm_base_interface xdg_wm_base_implementation = {
    .destroy = handle_destroy_xdg_wm_base,
    .create_positioner = handle_create_positioner_xdg_wm_base,
    .get_xdg_surface = handle_get_xdg_surface_xdg_wm_base,
    .pong = handle_pong_xdg_wm_base,
};

static void
handle_bind_xdg_wm_base(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    ww_assert(version <= SERVER_XDG_WM_BASE_VERSION);
    struct compositor *compositor = data;

    struct server_xdg_wm_base *server_xdg_wm_base = ww_alloc(1, sizeof(*server_xdg_wm_base));
    struct wl_resource *resource =
        wl_resource_create(client, &xdg_wm_base_interface, xdg_wm_base_interface.version, id);
    wl_resource_set_implementation(resource, &xdg_wm_base_implementation, server_xdg_wm_base,
                                   destroy_xdg_wm_base);

    server_xdg_wm_base->compositor = compositor;
    wl_list_init(&server_xdg_wm_base->surfaces);
}

/*
 *  Client (remote) code
 */

#define WL_COMPOSITOR_VERSION 5
#define WL_DATA_DEVICE_MANAGER_VERSION 3
#define WL_SUBCOMPOSITOR_VERSION 1
#define WL_SEAT_VERSION 8
#define WL_SHM_VERSION 1
#define WP_VIEWPORTER_VERSION 1
#define WP_SINGLE_PIXEL_BUFFER_VERSION 1
#define WP_TEARING_CONTROL_VERSION 1
#define XDG_WM_BASE_VERSION 5
#define ZWP_LINUX_DMABUF_VERSION 4
#define ZWP_POINTER_CONSTRAINTS_VERSION 1
#define ZWP_RELATIVE_POINTER_VERSION 1

#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 360

static void
send_pointer_leave(struct client_seat *seat) {
    ww_assert(seat);
    ww_assert(seat->pointer_focus);

    bool found_client = false;

    struct wl_resource *seat_resource;
    wl_resource_for_each(seat_resource, &seat->clients) {
        if (wl_resource_get_client(seat_resource) != wl_resource_get_client(seat->pointer_focus)) {
            continue;
        }

        struct server_seat *server_seat = wl_resource_get_user_data(seat_resource);

        struct wl_resource *pointer_resource;
        wl_resource_for_each(pointer_resource, &server_seat->pointers) {
            wl_pointer_send_leave(pointer_resource, next_serial(pointer_resource),
                                  seat->pointer_focus);
        }

        found_client = true;
    }

    if (!found_client) {
        LOG(LOG_ERROR, "failed to find client to send wl_pointer.leave to");
    }
    seat->pointer_focus = NULL;
}

static void
send_keyboard_leave(struct client_seat *seat) {
    ww_assert(seat);
    ww_assert(seat->keyboard_focus);

    bool found_client = false;

    struct wl_resource *seat_resource;
    wl_resource_for_each(seat_resource, &seat->clients) {
        if (wl_resource_get_client(seat_resource) != wl_resource_get_client(seat->keyboard_focus)) {
            continue;
        }

        struct server_seat *server_seat = wl_resource_get_user_data(seat_resource);

        struct wl_resource *keyboard_resource;
        wl_resource_for_each(keyboard_resource, &server_seat->keyboards) {
            wl_keyboard_send_modifiers(keyboard_resource, next_serial(keyboard_resource), 0, 0, 0,
                                       0);

            for (size_t i = 0; i < seat->num_pressed_keys; i++) {
                wl_keyboard_send_key(keyboard_resource, next_serial(keyboard_resource),
                                     current_time(), seat->pressed_keys[i],
                                     WL_KEYBOARD_KEY_STATE_RELEASED);
            }

            wl_keyboard_send_leave(keyboard_resource, next_serial(keyboard_resource),
                                   seat->keyboard_focus);
        }

        found_client = true;
    }

    if (!found_client) {
        LOG(LOG_ERROR, "failed to find client to send wl_keyboard.leave to");
    }

    seat->mods_depressed = 0;
    seat->mods_latched = 0;
    seat->mods_locked = 0;
    seat->group = 0;
    seat->num_pressed_keys = 0;

    seat->keyboard_focus = NULL;
}

static void
client_seat_destroy(struct client_seat *seat) {
    ww_assert(seat);

    if (seat->pointer) {
        wl_pointer_release(seat->pointer);
        if (seat->pointer_focus) {
            send_pointer_leave(seat);
        }
    }
    if (seat->relative_pointer) {
        zwp_relative_pointer_v1_destroy(seat->relative_pointer);
        seat->relative_pointer = NULL;
    }
    if (seat->keyboard) {
        wl_keyboard_release(seat->keyboard);
        if (seat->keyboard_focus) {
            send_keyboard_leave(seat);
        }
    }

    wl_global_destroy(seat->global);
    wl_seat_release(seat->wl);

    wl_list_remove(&seat->link);
    free(seat->str_name);
    free(seat);
}

static void
on_relative_pointer_relative_motion(void *data, struct zwp_relative_pointer_v1 *relative_pointer,
                                    uint32_t utime_hi, uint32_t utime_lo, wl_fixed_t dx,
                                    wl_fixed_t dy, wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel) {
    struct client_seat *seat = data;

    if (!seat || !seat->pointer_focus) {
        return;
    }

    struct wl_resource *seat_resource;
    wl_resource_for_each(seat_resource, &seat->clients) {
        if (wl_resource_get_client(seat_resource) != wl_resource_get_client(seat->pointer_focus)) {
            continue;
        }

        struct server_seat *server_seat = wl_resource_get_user_data(seat_resource);

        struct wl_resource *pointer_resource;
        wl_resource_for_each(pointer_resource, &server_seat->pointers) {
            struct server_pointer *server_pointer = wl_resource_get_user_data(pointer_resource);

            // TODO: multiply values according to sensitivity in config

            struct wl_resource *relative_pointer_resource;
            wl_resource_for_each(relative_pointer_resource, &server_pointer->relative_pointers) {
                zwp_relative_pointer_v1_send_relative_motion(
                    relative_pointer_resource, utime_hi, utime_lo, dx, dy, dx_unaccel, dy_unaccel);
            }
        }
    }
}

static const struct zwp_relative_pointer_v1_listener relative_pointer_listener = {
    .relative_motion = on_relative_pointer_relative_motion,
};

static void
on_pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                 struct wl_surface *wl_surface, wl_fixed_t surface_x, wl_fixed_t surface_y) {
    struct client_seat *seat = data;
    struct wl_resource *surface_resource = wl_surface_get_user_data(wl_surface);

    if (!surface_resource) {
        if (seat->pointer_focus) {
            send_pointer_leave(seat);
        }
        return;
    }

    seat->pointer_focus = surface_resource;

    bool found_client = false;

    struct wl_resource *seat_resource;
    wl_resource_for_each(seat_resource, &seat->clients) {
        if (wl_resource_get_client(seat_resource) != wl_resource_get_client(surface_resource)) {
            continue;
        }

        struct server_seat *server_seat = wl_resource_get_user_data(seat_resource);

        struct wl_resource *pointer_resource;
        wl_resource_for_each(pointer_resource, &server_seat->pointers) {
            wl_pointer_send_enter(pointer_resource, next_serial(pointer_resource), surface_resource,
                                  surface_x, surface_y);
        }

        found_client = true;
    }

    if (!found_client) {
        LOG(LOG_ERROR, "failed to find client to send wl_pointer.enter to");
    }
}

static void
on_pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                 struct wl_surface *wl_surface) {
    struct client_seat *seat = data;

    if (!wl_surface) {
        // We can receive a null wl_surface (e.g. when the surface is destroyed).
        ww_assert(!seat->pointer_focus);
        return;
    }

    struct wl_resource *surface_resource = wl_surface_get_user_data(wl_surface);

    if (!surface_resource) {
        ww_assert(!seat->pointer_focus);
        return;
    }

    ww_assert(seat->pointer_focus);

    send_pointer_leave(seat);
}

static void
on_pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t surface_x,
                  wl_fixed_t surface_y) {
    struct client_seat *seat = data;

    if (!seat->pointer_focus) {
        return;
    }

    bool found_client = false;

    struct wl_resource *seat_resource;
    wl_resource_for_each(seat_resource, &seat->clients) {
        if (wl_resource_get_client(seat_resource) != wl_resource_get_client(seat->pointer_focus)) {
            continue;
        }

        struct server_seat *server_seat = wl_resource_get_user_data(seat_resource);

        struct wl_resource *pointer_resource;
        wl_resource_for_each(pointer_resource, &server_seat->pointers) {
            wl_pointer_send_motion(pointer_resource, current_time(), surface_x, surface_y);
        }

        found_client = true;
    }

    if (!found_client) {
        LOG(LOG_ERROR, "failed to find client to send wl_pointer.motion to");
    }
}

static void
on_pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time,
                  uint32_t button, uint32_t state) {
    struct client_seat *seat = data;

    if (!seat->pointer_focus) {
        return;
    }

    bool found_client = false;

    struct wl_resource *seat_resource;
    wl_resource_for_each(seat_resource, &seat->clients) {
        if (wl_resource_get_client(seat_resource) != wl_resource_get_client(seat->pointer_focus)) {
            continue;
        }

        struct server_seat *server_seat = wl_resource_get_user_data(seat_resource);

        struct wl_resource *pointer_resource;
        wl_resource_for_each(pointer_resource, &server_seat->pointers) {
            wl_pointer_send_button(pointer_resource, next_serial(pointer_resource), current_time(),
                                   button, state);
        }

        found_client = true;
    }

    if (!found_client) {
        LOG(LOG_ERROR, "failed to find client to send wl_pointer.button to");
    }
}

static void
on_pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis,
                wl_fixed_t value) {
    struct client_seat *seat = data;

    if (!seat->pointer_focus) {
        return;
    }

    bool found_client = false;

    struct wl_resource *seat_resource;
    wl_resource_for_each(seat_resource, &seat->clients) {
        if (wl_resource_get_client(seat_resource) != wl_resource_get_client(seat->pointer_focus)) {
            continue;
        }

        struct server_seat *server_seat = wl_resource_get_user_data(seat_resource);

        struct wl_resource *pointer_resource;
        wl_resource_for_each(pointer_resource, &server_seat->pointers) {
            wl_pointer_send_axis(pointer_resource, current_time(), axis, value);
        }

        found_client = true;
    }

    if (!found_client) {
        LOG(LOG_ERROR, "failed to find client to send wl_pointer.axis to");
    }
}

static void
on_pointer_frame(void *data, struct wl_pointer *pointer) {
    // Do nothing. GLFW uses very limited pointer events.
}

static void
on_pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t axis_source) {
    // Do nothing. GLFW uses very limited pointer events.
}

static void
on_pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis) {
    // Do nothing. GLFW uses very limited pointer events.
}

static void
on_pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete) {
    // Do nothing. GLFW uses very limited pointer events.
}

static void
on_pointer_axis_value120(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t value120) {
    // Do nothing. GLFW uses very limited pointer events.
}

static void
on_pointer_axis_relative_direction(void *data, struct wl_pointer *pointer, uint32_t axis,
                                   uint32_t direction) {
    // Do nothing. GLFW uses very limited pointer events.
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = on_pointer_enter,
    .leave = on_pointer_leave,
    .motion = on_pointer_motion,
    .button = on_pointer_button,
    .axis = on_pointer_axis,
    .frame = on_pointer_frame,
    .axis_source = on_pointer_axis_source,
    .axis_stop = on_pointer_axis_stop,
    .axis_discrete = on_pointer_axis_discrete,
    .axis_value120 = on_pointer_axis_value120,
    .axis_relative_direction = on_pointer_axis_relative_direction,
};

static void
on_keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd,
                   uint32_t size) {
    struct client_seat *seat = data;
    // TODO: do something with this for processing config keybindings
    // TODO: forward to server seat if needed (no user-specified keymap)
}

static void
on_keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                  struct wl_surface *wl_surface, struct wl_array *keys) {
    struct client_seat *seat = data;
    struct wl_resource *surface_resource = wl_surface_get_user_data(wl_surface);

    if (!surface_resource) {
        if (seat->keyboard_focus) {
            send_keyboard_leave(seat);
        }
        return;
    }

    seat->keyboard_focus = surface_resource;

    seat->num_pressed_keys = 0;
    uint32_t *key_ptr;
    wl_array_for_each(key_ptr, keys) {
        seat->pressed_keys[seat->num_pressed_keys++] = *key_ptr;
    }

    bool found_client = false;

    struct wl_resource *seat_resource;
    wl_resource_for_each(seat_resource, &seat->clients) {
        if (wl_resource_get_client(seat_resource) != wl_resource_get_client(surface_resource)) {
            continue;
        }

        struct server_seat *server_seat = wl_resource_get_user_data(seat_resource);

        struct wl_resource *keyboard_resource;
        wl_resource_for_each(keyboard_resource, &server_seat->keyboards) {
            wl_keyboard_send_enter(keyboard_resource, next_serial(keyboard_resource),
                                   surface_resource, keys);
        }

        found_client = true;
    }

    if (!found_client) {
        LOG(LOG_ERROR, "failed to find client to send wl_keybaord.enter to");
    }
}

static void
on_keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                  struct wl_surface *wl_surface) {
    struct client_seat *seat = data;

    if (!wl_surface) {
        // We can receive a leave event for a null surface (e.g. when the surface is destroyed).
        ww_assert(!seat->keyboard_focus);
        return;
    }

    struct wl_resource *surface_resource = wl_surface_get_user_data(wl_surface);

    if (!surface_resource) {
        ww_assert(!seat->keyboard_focus);
        return;
    }

    ww_assert(seat->keyboard_focus);

    send_keyboard_leave(seat);
}

static void
on_keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time,
                uint32_t key, uint32_t state) {
    struct client_seat *seat = data;

    if (!seat->keyboard_focus) {
        return;
    }

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        seat->pressed_keys[seat->num_pressed_keys++] = key;
    } else {
        for (size_t i = 0; i < seat->num_pressed_keys; i++) {
            if (seat->pressed_keys[i] == key) {
                memmove(seat->pressed_keys + i, seat->pressed_keys + i + 1,
                        sizeof(uint32_t) * (seat->num_pressed_keys - i - 1));
                seat->num_pressed_keys--;
                goto send_event;
            }
        }
        LOG(LOG_ERROR, "received spurious key release event (keycode %" PRIu32 ")", key);
        return;
    }

send_event:;

    bool found_client = false;

    struct wl_resource *seat_resource;
    wl_resource_for_each(seat_resource, &seat->clients) {
        if (wl_resource_get_client(seat_resource) != wl_resource_get_client(seat->keyboard_focus)) {
            continue;
        }

        struct server_seat *server_seat = wl_resource_get_user_data(seat_resource);

        struct wl_resource *keyboard_resource;
        wl_resource_for_each(keyboard_resource, &server_seat->keyboards) {
            wl_keyboard_send_key(keyboard_resource, next_serial(keyboard_resource), current_time(),
                                 key, state);
        }

        found_client = true;
    }

    if (!found_client) {
        LOG(LOG_ERROR, "failed to find client to send wl_keyboard.key to");
    }
}

static void
on_keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                      uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked,
                      uint32_t group) {
    struct client_seat *seat = data;

    if (!seat->keyboard_focus) {
        return;
    }

    seat->mods_depressed = mods_depressed;
    seat->mods_latched = mods_latched;
    seat->mods_locked = mods_locked;
    seat->group = group;

    bool found_client = false;

    struct wl_resource *seat_resource;
    wl_resource_for_each(seat_resource, &seat->clients) {
        if (wl_resource_get_client(seat_resource) != wl_resource_get_client(seat->keyboard_focus)) {
            return;
        }

        struct server_seat *server_seat = wl_resource_get_user_data(seat_resource);

        struct wl_resource *keyboard_resource;
        wl_resource_for_each(keyboard_resource, &server_seat->keyboards) {
            wl_keyboard_send_modifiers(keyboard_resource, next_serial(keyboard_resource),
                                       mods_depressed, mods_latched, mods_locked, group);
        }

        found_client = true;
    }

    if (!found_client) {
        LOG(LOG_ERROR, "failed to find client to send wl_keyboard.modifiers to");
    }
}

static void
on_keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay) {
    // Unused.
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = on_keyboard_keymap,
    .enter = on_keyboard_enter,
    .leave = on_keyboard_leave,
    .key = on_keyboard_key,
    .modifiers = on_keyboard_modifiers,
    .repeat_info = on_keyboard_repeat_info,
};

static void
on_seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities) {
    struct client_seat *seat = data;
    struct compositor *compositor = seat->compositor;

    if (seat->caps == capabilities) {
        return;
    }
    seat->caps = capabilities;

    struct wl_resource *seat_resource;
    wl_resource_for_each(seat_resource, &seat->clients) {
        struct server_seat *server_seat = wl_resource_get_user_data(seat_resource);
        server_seat_handle_caps(server_seat, capabilities);
    }

    if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
        seat->pointer = wl_seat_get_pointer(seat->wl);
        wl_pointer_add_listener(seat->pointer, &pointer_listener, seat);

        seat->relative_pointer = zwp_relative_pointer_manager_v1_get_relative_pointer(
            compositor->remote.relative_pointer_manager, seat->pointer);
        zwp_relative_pointer_v1_add_listener(seat->relative_pointer, &relative_pointer_listener,
                                             seat);
    } else {
        if (seat->pointer) {
            wl_pointer_release(seat->pointer);
            seat->pointer = NULL;
        }

        if (seat->relative_pointer) {
            zwp_relative_pointer_v1_destroy(seat->relative_pointer);
            seat->relative_pointer = NULL;
        }
    }

    if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
        seat->keyboard = wl_seat_get_keyboard(seat->wl);
        wl_keyboard_add_listener(seat->keyboard, &keyboard_listener, seat);
    } else {
        if (seat->keyboard) {
            wl_keyboard_release(seat->keyboard);
            seat->keyboard = NULL;
        }
    }
}

static void
on_seat_name(void *data, struct wl_seat *wl_seat, const char *name) {
    struct client_seat *seat = data;

    seat->str_name = strdup(name);
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = on_seat_capabilities,
    .name = on_seat_name,
};

static void
on_shm_format(void *data, struct wl_shm *shm, uint32_t format) {
    struct compositor *compositor = data;

    uint32_t *format_ptr = wl_array_add(&compositor->remote.shm_formats, sizeof(uint32_t));
    *format_ptr = format;
}

static const struct wl_shm_listener shm_listener = {
    .format = on_shm_format,
};

static void
on_registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
                   uint32_t version) {
    struct compositor *compositor = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor->remote.compositor =
            wl_registry_bind(registry, name, &wl_compositor_interface, WL_COMPOSITOR_VERSION);
    } else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
        compositor->remote.subcompositor =
            wl_registry_bind(registry, name, &wl_subcompositor_interface, WL_SUBCOMPOSITOR_VERSION);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        compositor->remote.shm =
            wl_registry_bind(registry, name, &wl_shm_interface, WL_SHM_VERSION);
        wl_shm_add_listener(compositor->remote.shm, &shm_listener, compositor);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        compositor->remote.data_device_manager = wl_registry_bind(
            registry, name, &wl_data_device_manager_interface, WL_DATA_DEVICE_MANAGER_VERSION);
    } else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        compositor->remote.linux_dmabuf = wl_registry_bind(
            registry, name, &zwp_linux_dmabuf_v1_interface, ZWP_LINUX_DMABUF_VERSION);
    } else if (strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) {
        compositor->remote.pointer_constraints = wl_registry_bind(
            registry, name, &zwp_pointer_constraints_v1_interface, ZWP_POINTER_CONSTRAINTS_VERSION);
    } else if (strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
        compositor->remote.relative_pointer_manager =
            wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface,
                             ZWP_RELATIVE_POINTER_VERSION);
    } else if (strcmp(interface, wp_single_pixel_buffer_manager_v1_interface.name) == 0) {
        compositor->remote.single_pixel_buffer_manager =
            wl_registry_bind(registry, name, &wp_single_pixel_buffer_manager_v1_interface,
                             WP_SINGLE_PIXEL_BUFFER_VERSION);
    } else if (strcmp(interface, wp_tearing_control_v1_interface.name) == 0) {
        compositor->remote.tearing_control_manager = wl_registry_bind(
            registry, name, &wp_tearing_control_v1_interface, WP_TEARING_CONTROL_VERSION);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        compositor->remote.viewporter =
            wl_registry_bind(registry, name, &wp_viewporter_interface, WP_VIEWPORTER_VERSION);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        compositor->remote.xdg_wm_base =
            wl_registry_bind(registry, name, &xdg_wm_base_interface, XDG_WM_BASE_VERSION);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        struct client_seat *seat = ww_alloc(1, sizeof(*seat));
        wl_list_insert(&compositor->remote.seats, &seat->link);
        seat->compositor = compositor;

        seat->wl = wl_registry_bind(registry, name, &wl_seat_interface, WL_SEAT_VERSION);
        seat->name = name;

        wl_list_init(&seat->clients);
        seat->global = wl_global_create(compositor->display, &wl_seat_interface,
                                        SERVER_WL_SEAT_VERSION, seat, &handle_bind_wl_seat);

        wl_seat_add_listener(seat->wl, &seat_listener, seat);
    }
}

static void
on_registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    struct compositor *compositor = data;

    struct client_seat *seat, *tmp;
    wl_list_for_each_safe (seat, tmp, &compositor->remote.seats, link) {
        if (seat->name == name) {
            client_seat_destroy(seat);
            return;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = on_registry_global,
    .global_remove = on_registry_global_remove,
};

static void
on_xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
    struct compositor *compositor = data;

    xdg_surface_ack_configure(xdg_surface, serial);

    xdg_surface_set_window_geometry(compositor->remote.xdg_surface, 0, 0,
                                    compositor->remote.win_width, compositor->remote.win_height);

    if (compositor->remote.viewport) {
        wp_viewport_set_destination(compositor->remote.viewport, compositor->remote.win_width,
                                    compositor->remote.win_height);
    }
    if (compositor->remote.surface) {
        wl_surface_commit(compositor->remote.surface);
    }
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = on_xdg_surface_configure,
};

static void
on_xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel, int32_t width,
                          int32_t height, struct wl_array *states) {
    struct compositor *compositor = data;

    // Provide sample sizes if the compositor doesn't.
    if (width == 0) {
        width = DEFAULT_WIDTH;
    }
    if (height == 0) {
        height = DEFAULT_HEIGHT;
    }

    compositor->remote.win_width = width;
    compositor->remote.win_height = height;
}

static void
on_xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel) {
    struct compositor *compositor = data;

    destroy_remote_window(compositor);
}

static void
on_xdg_toplevel_configure_bounds(void *data, struct xdg_toplevel *xdg_toplevel, int32_t width,
                                 int32_t height) {
    struct compositor *compositor = data;

    if (width == 0 || height == 0) {
        return;
    }
    compositor->remote.win_width = width;
    compositor->remote.win_height = height;
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
create_remote_buffer(struct compositor *compositor, const uint32_t color[4]) {
    ww_assert(compositor);
    ww_assert(!compositor->remote.buffer);

    compositor->remote.buffer = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(
        compositor->remote.single_pixel_buffer_manager, color[0], color[1], color[2], color[3]);
}

static void
destroy_remote_buffer(struct compositor *compositor) {
    ww_assert(compositor);
    ww_assert(compositor->remote.buffer);

    wl_buffer_destroy(compositor->remote.buffer);
    compositor->remote.buffer = NULL;
}

static void
create_remote_window(struct compositor *compositor) {
    ww_assert(compositor);
    ww_assert(compositor->remote.buffer);
    ww_assert(!compositor->remote.surface);
    ww_assert(!compositor->remote.viewport);
    ww_assert(!compositor->remote.xdg_surface);
    ww_assert(!compositor->remote.xdg_toplevel);

    // Setup the XDG toplevel.
    compositor->remote.surface = wl_compositor_create_surface(compositor->remote.compositor);
    wl_surface_set_user_data(compositor->remote.surface, NULL);

    compositor->remote.xdg_surface =
        xdg_wm_base_get_xdg_surface(compositor->remote.xdg_wm_base, compositor->remote.surface);
    xdg_surface_add_listener(compositor->remote.xdg_surface, &xdg_surface_listener, compositor);

    compositor->remote.xdg_toplevel = xdg_surface_get_toplevel(compositor->remote.xdg_surface);
    xdg_toplevel_add_listener(compositor->remote.xdg_toplevel, &xdg_toplevel_listener, compositor);

    wl_surface_commit(compositor->remote.surface);
    wl_display_roundtrip(compositor->remote.display);

    // Setup the single color buffer for the XDG surface.
    wl_surface_attach(compositor->remote.surface, compositor->remote.buffer, 0, 0);
    compositor->remote.viewport =
        wp_viewporter_get_viewport(compositor->remote.viewporter, compositor->remote.surface);
    wp_viewport_set_destination(compositor->remote.viewport, compositor->remote.win_width,
                                compositor->remote.win_height);

    wl_surface_commit(compositor->remote.surface);
    wl_display_roundtrip(compositor->remote.display);

    // Create the wl_output global representing the XDG toplevel.
    compositor->globals.wl_output =
        wl_global_create(compositor->display, &wl_output_interface, SERVER_WL_OUTPUT_VERSION,
                         compositor, &handle_bind_wl_output);
}

static void
destroy_remote_window(struct compositor *compositor) {
    ww_assert(compositor);
    ww_assert(compositor->remote.surface);
    ww_assert(compositor->remote.viewport);
    ww_assert(compositor->remote.xdg_surface);
    ww_assert(compositor->remote.xdg_toplevel);

    // Destroy the subsurfaces associated with the toplevel.
    struct wl_resource *surface_resource;
    wl_resource_for_each(surface_resource, &compositor->globals.surfaces) {
        struct server_surface *server_surface = wl_resource_get_user_data(surface_resource);
        if (server_surface->subsurface) {
            wl_subsurface_destroy(server_surface->subsurface);
            server_surface->subsurface = NULL;
        }
    }

    // Destroy the XDG toplevel.
    xdg_toplevel_destroy(compositor->remote.xdg_toplevel);
    compositor->remote.xdg_toplevel = NULL;
    xdg_surface_destroy(compositor->remote.xdg_surface);
    compositor->remote.xdg_surface = NULL;

    compositor->remote.win_width = 0;
    compositor->remote.win_height = 0;

    // Destroy the surface.
    wp_viewport_destroy(compositor->remote.viewport);
    compositor->remote.viewport = NULL;

    wl_surface_destroy(compositor->remote.surface);
    compositor->remote.surface = NULL;

    // Destroy the wl_output global that represents the remote XDG toplevel.
    wl_global_destroy(compositor->globals.wl_output);
    compositor->globals.wl_output = NULL;
}

static int
process_remote_display(int fd, uint32_t mask, void *data) {
    struct compositor *compositor = data;

    // Adapted from wlroots' Wayland backend code.
    // backend/wayland/backend.c (dispatch_events)

    // Handle any errors.
    if (mask & WL_EVENT_HANGUP) {
        wl_display_terminate(compositor->display);
        return 0;
    }
    if (mask & WL_EVENT_ERROR) {
        LOG(LOG_ERROR, "failed to read events from remote wayland display");
        wl_display_terminate(compositor->display);
        return 0;
    }

    // Handle reading and/or writing events.
    if (mask & WL_EVENT_WRITABLE) {
        wl_display_flush(compositor->remote.display);
    }

    int ret = 0;
    if (mask & WL_EVENT_READABLE) {
        ret = wl_display_dispatch(compositor->remote.display);
    } else if (mask == 0) {
        ret = wl_display_dispatch_pending(compositor->remote.display);
        wl_display_flush(compositor->remote.display);
    }

    if (ret < 0) {
        LOG(LOG_ERROR, "failed to dispatch remote wayland display");
        wl_display_terminate(compositor->display);
        return 0;
    }
    return ret > 1;
}

static int
process_sigint(int signal, void *data) {
    struct compositor *compositor = data;

    wl_display_terminate(compositor->display);
    return 0;
}

/*
 *  Public API
 */

struct compositor *
compositor_create() {
    struct compositor *compositor = ww_alloc(1, sizeof(*compositor));

    compositor->display = wl_display_create();
    compositor->socket_name = wl_display_add_socket_auto(compositor->display);

    compositor->remote.display = wl_display_connect(NULL);
    if (!compositor->remote.display) {
        LOG(LOG_ERROR, "failed to connect to remote wayland display");
        goto fail_remote_display;
    }

    struct wl_event_loop *event_loop = wl_display_get_event_loop(compositor->display);
    compositor->src_remote =
        wl_event_loop_add_fd(event_loop, wl_display_get_fd(compositor->remote.display),
                             WL_EVENT_READABLE, process_remote_display, compositor);
    wl_event_source_check(compositor->src_remote);
    compositor->src_sigint =
        wl_event_loop_add_signal(event_loop, SIGINT, process_sigint, compositor);

    wl_list_init(&compositor->remote.seats);
    wl_array_init(&compositor->remote.shm_formats);

    wl_list_init(&compositor->globals.surfaces);

    compositor->remote.registry = wl_display_get_registry(compositor->remote.display);
    wl_registry_add_listener(compositor->remote.registry, &registry_listener, compositor);
    wl_display_roundtrip(compositor->remote.display);

    // Mandatory remote globals
    if (!compositor->remote.compositor) {
        LOG(LOG_ERROR, "host compositor does not provide wl_compositor global");
        goto fail_remote_registry;
    }
    if (!compositor->remote.subcompositor) {
        LOG(LOG_ERROR, "host compositor does not provide wl_subcompositor global");
        goto fail_remote_registry;
    }
    if (!compositor->remote.shm) {
        LOG(LOG_ERROR, "host compositor does not provide wl_shm global");
        goto fail_remote_registry;
    }
    if (!compositor->remote.pointer_constraints) {
        LOG(LOG_ERROR, "host compositor does not provide zwp_pointer_constraints_v1 global");
        goto fail_remote_registry;
    }
    if (!compositor->remote.relative_pointer_manager) {
        LOG(LOG_ERROR, "host compositor does not provide zwp_relative_pointer_manager_v1 global");
        goto fail_remote_registry;
    }
    if (!compositor->remote.single_pixel_buffer_manager) {
        LOG(LOG_ERROR, "host compositor does not provide wp_single_pixel_buffer_manager_v1 global");
        goto fail_remote_registry;
    }
    if (!compositor->remote.viewporter) {
        LOG(LOG_ERROR, "host compositor does not provide wp_viewporter global");
        goto fail_remote_registry;
    }
    if (!compositor->remote.xdg_wm_base) {
        LOG(LOG_ERROR, "host compositor does not provide xdg_wm_base global");
        goto fail_remote_registry;
    }

    // Optional remote globals
    if (!compositor->remote.data_device_manager) {
        LOG(LOG_INFO, "host compositor does not provide wl_data_device_manager global");
    }
    if (!compositor->remote.tearing_control_manager) {
        LOG(LOG_INFO, "host compositor does not provide wp_tearing_control_manager_v1 global");
    }

    // Server globals
    compositor->globals.wl_compositor =
        wl_global_create(compositor->display, &wl_compositor_interface,
                         SERVER_WL_COMPOSITOR_VERSION, compositor, handle_bind_wl_compositor);
    compositor->globals.wl_shm =
        wl_global_create(compositor->display, &wl_shm_interface, SERVER_WL_SHM_VERSION, compositor,
                         handle_bind_wl_shm);
    compositor->globals.relative_pointer =
        wl_global_create(compositor->display, &zwp_relative_pointer_manager_v1_interface,
                         SERVER_RELATIVE_POINTER_VERSION, compositor, handle_bind_relative_pointer);
    compositor->globals.linux_dmabuf =
        wl_global_create(compositor->display, &zwp_linux_dmabuf_v1_interface,
                         SERVER_LINUX_DMABUF_VERSION, compositor, handle_bind_linux_dmabuf);
    compositor->globals.xdg_decoration =
        wl_global_create(compositor->display, &zxdg_decoration_manager_v1_interface,
                         SERVER_XDG_DECORATION_VERSION, compositor, handle_bind_xdg_decoration);
    compositor->globals.xdg_wm_base =
        wl_global_create(compositor->display, &xdg_wm_base_interface, SERVER_XDG_WM_BASE_VERSION,
                         compositor, handle_bind_xdg_wm_base);
    // TODO: data device manager, tearing control

    // Final setup (server + client)
    // TODO: configurable background color
    uint32_t colors[4] = {0, 0, 0, UINT32_MAX};
    create_remote_buffer(compositor, colors);
    create_remote_window(compositor);

    return compositor;

fail_remote_registry:;
    struct client_seat *seat, *tmp;
    wl_list_for_each_safe (seat, tmp, &compositor->remote.seats, link) {
        client_seat_destroy(seat);
    }

    if (compositor->remote.compositor) {
        wl_compositor_destroy(compositor->remote.compositor);
    }
    if (compositor->remote.subcompositor) {
        wl_subcompositor_destroy(compositor->remote.subcompositor);
    }
    if (compositor->remote.shm) {
        wl_shm_destroy(compositor->remote.shm);
    }
    if (compositor->remote.data_device_manager) {
        wl_data_device_manager_destroy(compositor->remote.data_device_manager);
    }
    if (compositor->remote.linux_dmabuf) {
        zwp_linux_dmabuf_v1_destroy(compositor->remote.linux_dmabuf);
    }
    if (compositor->remote.pointer_constraints) {
        zwp_pointer_constraints_v1_destroy(compositor->remote.pointer_constraints);
    }
    if (compositor->remote.relative_pointer_manager) {
        zwp_relative_pointer_manager_v1_destroy(compositor->remote.relative_pointer_manager);
    }
    if (compositor->remote.single_pixel_buffer_manager) {
        wp_single_pixel_buffer_manager_v1_destroy(compositor->remote.single_pixel_buffer_manager);
    }
    if (compositor->remote.tearing_control_manager) {
        wp_tearing_control_manager_v1_destroy(compositor->remote.tearing_control_manager);
    }
    if (compositor->remote.viewporter) {
        wp_viewporter_destroy(compositor->remote.viewporter);
    }
    if (compositor->remote.xdg_wm_base) {
        xdg_wm_base_destroy(compositor->remote.xdg_wm_base);
    }
    wl_registry_destroy(compositor->remote.registry);
    wl_display_disconnect(compositor->remote.display);

    wl_event_source_remove(compositor->src_remote);
    wl_event_source_remove(compositor->src_sigint);

    wl_array_release(&compositor->remote.shm_formats);

fail_remote_display:
    wl_display_destroy(compositor->display);

    free(compositor);
    return NULL;
}

void
compositor_destroy(struct compositor *compositor) {
    ww_assert(compositor);

    wl_event_source_remove(compositor->src_remote);
    wl_event_source_remove(compositor->src_sigint);

    // Client (remote)
    struct client_seat *seat, *tmp;
    wl_list_for_each_safe (seat, tmp, &compositor->remote.seats, link) {
        client_seat_destroy(seat);
    }

    // Client globals
    wl_compositor_destroy(compositor->remote.compositor);
    wl_subcompositor_destroy(compositor->remote.subcompositor);
    wl_shm_destroy(compositor->remote.shm);
    zwp_linux_dmabuf_v1_destroy(compositor->remote.linux_dmabuf);
    zwp_pointer_constraints_v1_destroy(compositor->remote.pointer_constraints);
    zwp_relative_pointer_manager_v1_destroy(compositor->remote.relative_pointer_manager);
    wp_single_pixel_buffer_manager_v1_destroy(compositor->remote.single_pixel_buffer_manager);
    wp_viewporter_destroy(compositor->remote.viewporter);
    xdg_wm_base_destroy(compositor->remote.xdg_wm_base);
    if (compositor->remote.data_device_manager) {
        wl_data_device_manager_destroy(compositor->remote.data_device_manager);
    }
    if (compositor->remote.tearing_control_manager) {
        wp_tearing_control_manager_v1_destroy(compositor->remote.tearing_control_manager);
    }

    // Remote objects (toplevel, registry)
    if (compositor->remote.xdg_toplevel) {
        destroy_remote_window(compositor);
    }
    destroy_remote_buffer(compositor);

    wl_registry_destroy(compositor->remote.registry);

    // Server
    wl_display_destroy_clients(compositor->display);

    wl_global_destroy(compositor->globals.wl_compositor);
    wl_global_destroy(compositor->globals.wl_shm);
    wl_global_destroy(compositor->globals.relative_pointer);
    wl_global_destroy(compositor->globals.linux_dmabuf);
    wl_global_destroy(compositor->globals.xdg_decoration);
    wl_global_destroy(compositor->globals.xdg_wm_base);

    wl_display_destroy(compositor->display);

    // Wait to disconnect from the remote display until all of the server objects have run their
    // teardown code.
    wl_display_disconnect(compositor->remote.display);

    wl_array_release(&compositor->remote.shm_formats);
    free(compositor);
}

int
main() {
    struct compositor *c = compositor_create();
    if (c) {
        wl_display_run(c->display);
    }
    compositor_destroy(c);
    return 0;
}
