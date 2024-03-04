#ifndef WAYWALL_SERVER_REMOTE_BUFFER_H
#define WAYWALL_SERVER_REMOTE_BUFFER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <wayland-server-core.h>

#define REMOTE_BUFFER_MAX_COLORS 64

struct server;

struct remote_buffer {
    struct wl_buffer *wl;

    uint32_t width, height, stride;
    size_t offset;
    ssize_t rc;
};

struct remote_buffer_manager {
    struct wl_shm_pool *pool;
    char *data;
    int32_t fd;
    size_t size, ptr;

    struct {
        struct remote_buffer buf;
        uint32_t argb;
    } colors[REMOTE_BUFFER_MAX_COLORS];

    struct wl_listener on_display_destroy;
};

struct remote_buffer_manager *remote_buffer_manager_create(struct server *server);
struct wl_buffer *remote_buffer_manager_color(struct remote_buffer_manager *manager,
                                              uint8_t rgba[static 4]);
void remote_buffer_deref(struct wl_buffer *buffer);

#endif
