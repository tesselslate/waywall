#define _GNU_SOURCE
#include <sys/mman.h>
#undef _GNU_SOURCE

#include "server/remote_buffer.h"
#include "server/server.h"
#include "util.h"
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server-core.h>

#define POOL_INITIAL_SIZE 16384

static inline void
assign_rgba(char *dst, uint8_t src[static 4]) {
    dst[0] = src[2];
    dst[1] = src[1];
    dst[2] = src[0];
    dst[3] = src[3];
}

static int
expand(struct remote_buffer_manager *manager, size_t min) {
    ww_assert(min < SIZE_MAX / 2);

    size_t prev = manager->size;
    do {
        manager->size *= 2;
    } while (manager->size < min);

    if (ftruncate(manager->fd, manager->size) == -1) {
        ww_log_errno(LOG_ERROR, "failed to reallocate %zu bytes for shm buffers", manager->size);
        goto fail_truncate;
    }

    char *new_data = mmap(NULL, manager->size, PROT_READ | PROT_WRITE, MAP_SHARED, manager->fd, 0);
    if (new_data == MAP_FAILED) {
        ww_log_errno(LOG_ERROR, "failed to mmap new shm pool");
        goto fail_mmap;
    }

    ww_assert(munmap(manager->pool, prev) == 0);
    manager->data = new_data;

    return 0;

fail_mmap:
fail_truncate:
    manager->size = prev;
    return 1;
}

static void
make_color(struct remote_buffer_manager *manager, struct remote_buffer *buf,
           uint8_t rgba[static 4]) {
    static_assert(sizeof(uint32_t) == 4);

    // Ensure there is space to create a fresh buffer.
    if (manager->ptr + 4 >= manager->size) {
        if (expand(manager, manager->ptr + 4) != 0) {
            ww_log(LOG_ERROR, "failed to expand shm pool for color buffer");
            return;
        }
    }

    buf->width = 1;
    buf->height = 1;
    buf->stride = 4;
    buf->offset = manager->ptr;
    manager->ptr += 4;

    assign_rgba(manager->data + buf->offset, rgba);

    buf->wl =
        wl_shm_pool_create_buffer(manager->pool, buf->offset, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
    ww_assert(buf->wl);

    wl_buffer_set_user_data(buf->wl, buf);
}

struct remote_buffer_manager *
remote_buffer_manager_create(struct server *server) {
    // Check that RGBA8 is a supported SHM buffer format, because it's what will be used.
    uint32_t *format;
    bool ok = false;
    wl_array_for_each(format, &server->backend.shm_formats) {
        if (*format == WL_SHM_FORMAT_ARGB8888) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        ww_log(LOG_ERROR, "RGBA8 is not a supported SHM format");
        return NULL;
    }

    struct remote_buffer_manager *manager = calloc(1, sizeof(*manager));
    if (!manager) {
        ww_log(LOG_ERROR, "failed to allocate remote_buffer_manager");
        return NULL;
    }

    manager->fd = memfd_create("waywall-shm", MFD_CLOEXEC);
    if (manager->fd == -1) {
        ww_log_errno(LOG_ERROR, "failed to create shm memfd");
        goto fail_memfd;
    }

    manager->size = POOL_INITIAL_SIZE;
    if (ftruncate(manager->fd, manager->size) == -1) {
        ww_log_errno(LOG_ERROR, "failed to expand shm memfd");
        goto fail_truncate;
    }

    manager->data = mmap(NULL, manager->size, PROT_READ | PROT_WRITE, MAP_SHARED, manager->fd, 0);
    if (manager->data == MAP_FAILED) {
        ww_log_errno(LOG_ERROR, "failed to mmap memfd");
        goto fail_mmap;
    }

    manager->pool = wl_shm_create_pool(server->backend.shm, manager->fd, manager->size);
    if (!manager->pool) {
        ww_log(LOG_ERROR, "failed to create wl_shm_pool");
        goto fail_shm_pool;
    }

    return manager;

fail_mmap:
fail_shm_pool:
fail_truncate:
    close(manager->fd);

fail_memfd:
    free(manager);
    return NULL;
}

struct wl_buffer *
remote_buffer_manager_color(struct remote_buffer_manager *manager, uint8_t rgba[static 4]) {
    uint32_t argb = (uint32_t)rgba[0] | ((uint32_t)rgba[1] << 8) | ((uint32_t)rgba[2] << 16) |
                    ((uint32_t)rgba[3] << 24);

    // Check for a buffer with this color first.
    for (size_t i = 0; i < STATIC_ARRLEN(manager->colors); i++) {
        if (manager->colors[i].argb != argb) {
            continue;
        }

        // In the case that the provided RGBA value is #00000000, the buffer may not be prepared
        // yet.
        if (!manager->colors[i].buf.wl) {
            make_color(manager, &manager->colors[i].buf, rgba);
        }
        struct wl_buffer *buffer = manager->colors[i].buf.wl;
        if (buffer) {
            manager->colors[i].buf.rc++;
        }
        return buffer;
    }

    // Otherwise, attempt to find a free slot.
    for (size_t i = 0; i < STATIC_ARRLEN(manager->colors); i++) {
        if (manager->colors[i].buf.rc > 0) {
            continue;
        }
        manager->colors[i].argb = argb;

        // If there is existing backing storage, rewrite it.
        struct remote_buffer *buf = &manager->colors[i].buf;
        if (buf->wl) {
            assign_rgba(manager->data + buf->offset, rgba);

            manager->colors[i].buf.rc++;
            return buf->wl;
        }

        // Otherwise, create a new buffer.
        make_color(manager, &manager->colors[i].buf, rgba);
        struct wl_buffer *buffer = manager->colors[i].buf.wl;
        if (buffer) {
            manager->colors[i].buf.rc++;
        }
        return buffer;
    }

    return NULL;
}

void
remote_buffer_manager_destroy(struct remote_buffer_manager *manager) {
    // All buffers should no longer be in use at this point.
    for (size_t i = 0; i < STATIC_ARRLEN(manager->colors); i++) {
        if (manager->colors[i].buf.rc > 0) {
            ww_panic("remote buffer still in use");
        }

        if (manager->colors[i].buf.wl) {
            wl_buffer_destroy(manager->colors[i].buf.wl);
        }
    }

    wl_shm_pool_destroy(manager->pool);
    ww_assert(munmap(manager->data, manager->size) == 0);
    close(manager->fd);
    free(manager);
}

void
remote_buffer_deref(struct wl_buffer *buffer) {
    struct remote_buffer *buf = wl_buffer_get_user_data(buffer);
    ww_assert(buf);
    ww_assert(buf->rc > 0);

    buf->rc--;
}
