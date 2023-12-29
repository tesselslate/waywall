#ifndef WAYWALL_COMPOSITOR_REMOTE_BUF_H
#define WAYWALL_COMPOSITOR_REMOTE_BUF_H

#include <wayland-server.h>

#define MAX_COLOR_REMOTE_BUFS 64

struct server;

struct remote_buf {
    struct wl_buffer *remote;

    bool on_heap;
    uint32_t data;

    uint32_t width, height, stride, format;
    uint32_t rc;
    size_t offset;
};

struct remote_buffer_manager {
    struct wl_shm *shm;           // owned by server
    struct wl_array *shm_formats; // owned by server
    struct wl_shm_pool *shm_pool;

    int pool_fd;
    size_t pool_size;
    size_t pool_ptr;
    uint32_t *pool_data;

    struct remote_buf color_buffers[MAX_COLOR_REMOTE_BUFS];

    struct wl_listener display_destroy;
};

struct remote_buffer_manager *remote_buffer_manager_create(struct server *server,
                                                           struct wl_shm *shm);

/*
 *  Decrements the reference count for the given buffer, allowing its storage to be reused later
 *  for new buffers.
 */
void remote_buffer_manager_deref(struct wl_buffer *buffer);

/*
 *  Provides a wl_buffer whose contents are a single pixel of the given RGBA color.
 */
struct wl_buffer *remote_buffer_manager_get_color(struct remote_buffer_manager *manager, uint8_t r,
                                                  uint8_t g, uint8_t b, uint8_t a);

#endif
