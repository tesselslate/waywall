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
    struct wl_list image_buffers; // image_buffer.link
};

struct remote_buffer_manager *remote_buffer_manager_create(struct server *server);
struct wl_buffer *remote_buffer_manager_color(struct remote_buffer_manager *manager,
                                              const uint8_t rgba[static 4]);

/**
 * Load a PNG image from a file path and create a Wayland buffer.
 * 
 * @param manager The remote buffer manager instance
 * @param png_path Path to the PNG file to load
 * @return A new wl_buffer containing the PNG image data, or NULL on failure
 * 
 * The returned buffer must be freed with remote_buffer_deref() when no longer needed.
 * The PNG is converted to ARGB8888 format for compatibility with Wayland SHM.
 */
struct wl_buffer *remote_buffer_manager_png(struct remote_buffer_manager *manager,
                                           const char *png_path);

/**
 * Load a PNG image from memory data and create a Wayland buffer.
 * 
 * @param manager The remote buffer manager instance
 * @param png_data Pointer to PNG file data in memory
 * @param data_size Size of the PNG data in bytes
 * @return A new wl_buffer containing the PNG image data, or NULL on failure
 * 
 * The returned buffer must be freed with remote_buffer_deref() when no longer needed.
 * The PNG is converted to ARGB8888 format for compatibility with Wayland SHM.
 * The input data is not modified and can be freed after this call returns.
 */
struct wl_buffer *remote_buffer_manager_png_data(struct remote_buffer_manager *manager,
                                                const uint8_t *png_data, size_t data_size);
void remote_buffer_manager_destroy(struct remote_buffer_manager *manager);
void remote_buffer_deref(struct wl_buffer *buffer);

#endif
