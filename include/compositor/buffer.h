#ifndef WAYWALL_COMPOSITOR_BUFFER_H
#define WAYWALL_COMPOSITOR_BUFFER_H

#include "util.h"
#include <stdint.h>

/*
 *  For wl_shm buffers, we only keep track of the dimensions. We don't care about their contents
 *  because they should never be used for actual window content. Keeping the wl_shm_pool fd open
 *  would require some more effort since we would need to reference-count the file descriptor.
 *
 *  NOTE: I think it might be technically possible that wl_shm buffers are used for window content
 *  if Minecraft is booted with software rasterization (e.g. llvmpipe.) Not sure, though. In any
 *  case, the performance would be bad enough that you wouldn't be running wall with it and
 *  recording.
 *
 *  For wp_linux_dmabuf buffers, we keep the DMABUF handles in struct buffer_data_dmabuf because
 *  they are owned by the buffers themselves (and not shared with other objects.) We also care
 *  about being able to read their contents, so that we can pass them along to screen recording
 *  software like OBS.
 */

enum buffer_type {
    BUFFER_SHM,
    BUFFER_DMABUF,
    BUFFER_INVALID,
};

struct dmabuf_plane {
    int fd;
    uint32_t offset, stride;
    uint64_t modifier;
};

#define MAX_PLANES 4

struct buffer_data_dmabuf {
    struct dmabuf_plane planes[MAX_PLANES];
    int32_t width, height;
    uint32_t format, flags;
    uint8_t num_planes;
};

struct buffer_data_shm {
    int32_t width, height;
};

struct server_buffer {
    struct wl_resource *resource;
    struct wl_buffer *remote;

    enum buffer_type type;
    union {
        struct buffer_data_dmabuf dmabuf;
        struct buffer_data_shm shm;
    } data;
};

extern const struct wl_buffer_listener server_buffer_listener;
extern const struct wl_buffer_interface server_buffer_impl;

static inline int32_t
server_buffer_get_width(struct server_buffer *buffer) {
    return buffer->type == BUFFER_SHM      ? buffer->data.shm.width
           : buffer->type == BUFFER_DMABUF ? buffer->data.dmabuf.width
                                           : ww_unreachable();
}

static inline int32_t
server_buffer_get_height(struct server_buffer *buffer) {
    return buffer->type == BUFFER_SHM      ? buffer->data.shm.height
           : buffer->type == BUFFER_DMABUF ? buffer->data.dmabuf.width
                                           : ww_unreachable();
}

void server_buffer_create_invalid(struct wl_resource *buffer_resource);
void server_buffer_destroy(struct server_buffer *buffer);
struct server_buffer *server_buffer_from_resource(struct wl_resource *resource);

#endif
