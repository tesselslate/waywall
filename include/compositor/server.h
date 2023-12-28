#ifndef WAYWALL_COMPOSITOR_SERVER_H
#define WAYWALL_COMPOSITOR_SERVER_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wayland-server-core.h>

/*
 *  server contains much of the state used to implement the embedded Wayland compositor. It also
 *  contains state for communicating with a host session compositor.
 */
struct server {
    struct wl_display *display;
    const char *socket_name;

    struct wl_event_source *source_remote;
    struct wl_event_source *source_sigint;

    struct {
        struct wl_display *display;
        struct wl_registry *registry;

        struct wl_compositor *compositor;
        struct wl_subcompositor *subcompositor;
        struct wl_shm *shm;
        struct wp_single_pixel_buffer_manager_v1 *single_pixel_buffer_manager;
        struct wp_viewporter *viewporter;
        struct xdg_wm_base *xdg_wm_base;
        struct zwp_linux_dmabuf_v1 *linux_dmabuf;
        struct zwp_relative_pointer_manager_v1 *relative_pointer_manager;

        struct wl_list seats; // remote_seat.link

        uint32_t compositor_id;
        uint32_t subcompositor_id;
        uint32_t shm_id;
        uint32_t single_pixel_manager_id;
        uint32_t viewporter_id;
        uint32_t xdg_wm_base_id;
        uint32_t linux_dmabuf_id;
        uint32_t relative_pointer_manager_id;
    } remote;

    struct server_compositor *compositor;
    struct server_seat *seat;
};

/*
 *  server_create attempts to create a new `struct server` instance. Returns NULL on failure.
 */
struct server *server_create();

/*
 *  server_destroy destroys the given `server` instance. The `server` object is invalid after
 *  this function call.
 */
void server_destroy(struct server *server);

/*
 *  server_run starts running the Wayland event loop for the provided `server` instance. Returns
 *  true on graceful shutdown, false on error.
 */
bool server_run(struct server *server);

static inline uint32_t
current_time() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t ms = (uint64_t)(now.tv_sec * 1000) + (uint64_t)(now.tv_nsec / 1000000);
    return (uint32_t)ms;
}

static inline uint32_t
next_serial(struct wl_resource *resource) {
    return wl_display_next_serial(wl_client_get_display(wl_resource_get_client(resource)));
}

#endif
