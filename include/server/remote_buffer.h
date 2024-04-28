#ifndef WAYWALL_SERVER_REMOTE_BUFFER_H
#define WAYWALL_SERVER_REMOTE_BUFFER_H

#include "server/server.h"
#include <stddef.h>
#include <stdint.h>
#include <wayland-util.h>

struct remote_buffer_manager {
    struct wl_shm_pool *pool;
    char *data;
    int32_t fd;
    size_t size, ptr;

    struct wl_list color_pools; // color_pool.link
};

struct remote_buffer_manager *remote_buffer_manager_create(struct server *server);
struct wl_buffer *remote_buffer_manager_color(struct remote_buffer_manager *manager,
                                              const uint8_t rgba[static 4]);
void remote_buffer_manager_destroy(struct remote_buffer_manager *manager);
void remote_buffer_deref(struct wl_buffer *buffer);

#endif
