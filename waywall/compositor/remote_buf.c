#include "compositor/remote_buf.h"
#include "compositor/server.h"
#include "util.h"
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include <wayland-server.h>

/*
 *  remote_buffer_manager is an attempt to consolidate all of the logic with creating wl_buffers
 *  for our own use (and not other clients.)
 *    - It avoids the need to depend on wp_single_pixel_buffer_manager (which KWin does not have).
 *    - TODO: It allows for creating wl_shm-based buffers to store cursor images.
 *    - It may be useful for storing actual images (e.g. backgrounds, lock icons) later.
 *
 *  There are a fixed number of "slots" for each type of buffer. Each slot can contain a single
 *  wl_buffer and a refcount. These slots are stored in the `remote_buffer_manager` itself.
 *  Reference counting is used so that buffers with the same contents can be shared instead of
 *  being recreated repeatedly.
 *
 *  Since each slot is in the manager struct, we must "break out" any still-in-use buffers when the
 *  manager is destroyed and place them on the heap. Then, when the dereference function is called,
 *  it can clean them up if needed.
 *
 *  The buffer slots are all stored in one allocation instead of in separate allocations to improve
 *  performance and keep things simple (no hashmap), although the extra logic for buffer destruction
 *  after the manager is destroyed is not ideal.
 */

// TODO: error handling, better shm allocation

#define SHM_POOL_INITIAL_SIZE 8192

static void
expand_shm(struct remote_buffer_manager *manager) {
    size_t prev_size = manager->pool_size;
    manager->pool_size *= 2;

    if (ftruncate(manager->pool_fd, manager->pool_size) < 0) {
        LOG(LOG_ERROR, "failed to reallocate %zu bytes for shm handle", manager->pool_size);
        ww_unreachable();
    }

    if (munmap(manager->pool_data, prev_size) < 0) {
        LOG(LOG_ERROR, "failed to munmap shm handle");
        ww_unreachable();
    }

    manager->pool_data =
        mmap(NULL, manager->pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, manager->pool_fd, 0);
    if (manager->pool_data == MAP_FAILED) {
        LOG(LOG_ERROR, "failed to re-mmap shm handle");
        ww_unreachable();
    }
}

static int
open_shm(size_t initial_size) {
    // waywall-PID
    char name[] = "/waywall-XXXXXXXX";

    uint32_t pid = getpid();
    for (int i = 0; i < 8; i++) {
        name[i + 9] = "0123456789ABCDEF"[pid & 0xF];
        pid >>= 4;
    }

    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        LOG(LOG_ERROR, "failed to create shm handle at %s", name);
        return -1;
    }
    shm_unlink(name);

    if (ftruncate(fd, initial_size) < 0) {
        LOG(LOG_ERROR, "failed to allocate %zu bytes for shm handle", initial_size);
        close(fd);
        return -1;
    }

    return fd;
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct remote_buffer_manager *manager = wl_container_of(listener, manager, display_destroy);

    wl_shm_pool_destroy(manager->shm_pool);
    munmap(manager->pool_data, manager->pool_size);
    close(manager->pool_fd);

    for (size_t i = 0; i < MAX_COLOR_REMOTE_BUFS; i++) {
        struct remote_buf *buf = &manager->color_buffers[i];

        if (buf->remote) {
            // If the buffer is not in use anymore, it can be destroyed immediately.
            if (buf->rc == 0) {
                wl_buffer_destroy(buf->remote);
                continue;
            }

            // Otherwise, allocate space for the remote_buf on the heap and move it there. This
            // way, other code can dereference the buffer as normal and it will be freed when the
            // refcount hits 0.
            struct remote_buf *heap_buf = calloc(1, sizeof(*heap_buf));
            if (!heap_buf) {
                LOG(LOG_ERROR, "failed to allocate space for remote_buf on heap");
                wl_buffer_set_user_data(manager->color_buffers[i].remote, NULL);
                continue;
            }

            heap_buf->on_heap = true;
            heap_buf->rc = buf->rc;

            wl_buffer_set_user_data(manager->color_buffers[i].remote, heap_buf);
        }
    }

    free(manager);
}

struct remote_buffer_manager *
remote_buffer_manager_create(struct server *server, struct wl_shm *shm) {
    struct remote_buffer_manager *manager = calloc(1, sizeof(*manager));
    if (!manager) {
        LOG(LOG_ERROR, "failed to allocate remote_buffer_manager");
        return NULL;
    }

    // Ensure ARGB8888 exists as a format.
    uint32_t *format;
    bool ok = false;
    wl_array_for_each(format, &server->remote.shm_formats) {
        if (*format == WL_SHM_FORMAT_ARGB8888) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        free(manager);
        LOG(LOG_ERROR, "ARGB8888 not in the list of supported wl_shm formats");
        return NULL;
    }

    manager->shm = shm;
    manager->shm_formats = &server->remote.shm_formats;

    manager->pool_size = SHM_POOL_INITIAL_SIZE;
    manager->pool_fd = open_shm(manager->pool_size);
    if (manager->pool_fd < 0) {
        free(manager);
        return NULL;
    }

    manager->pool_data =
        mmap(NULL, manager->pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, manager->pool_fd, 0);
    if (manager->pool_data == MAP_FAILED) {
        close(manager->pool_fd);
        free(manager);
        LOG(LOG_ERROR, "failed to mmap shm handle");
        return NULL;
    }

    manager->shm_pool = wl_shm_create_pool(manager->shm, manager->pool_fd, manager->pool_size);
    ww_assert(manager->shm_pool);

    manager->display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &manager->display_destroy);

    return manager;
}

void
remote_buffer_manager_deref(struct wl_buffer *buffer) {
    struct remote_buf *buf = wl_buffer_get_user_data(buffer);

    if (!buf) {
        LOG(LOG_ERROR,
            "attempted to dereference buffer with no associated remote_buf (allocation failure?)");
        return;
    }

    ww_assert(buf->rc);
    buf->rc--;

    if (buf->rc == 0 && buf->on_heap) {
        wl_buffer_destroy(buffer);
        free(buf);
    }
}

struct wl_buffer *
remote_buffer_manager_get_color(struct remote_buffer_manager *manager, uint8_t r, uint8_t g,
                                uint8_t b, uint8_t a) {
    static_assert(sizeof(uint32_t) == 4, "uint32_t maps to ARGB8888");
    uint32_t argb = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);

    for (size_t i = 0; i < MAX_COLOR_REMOTE_BUFS; i++) {
        struct remote_buf *buf = &manager->color_buffers[i];

        // If there is an already existing buffer with the same color, use it.
        // Otherwise, we need to repurpose another unused buffer or make a new one.
        if (buf->data == argb && buf->remote) {
            buf->rc++;
            return buf->remote;
        }

        if (buf->rc > 0) {
            continue;
        }

        // If this slot already has a created wl_buffer, simply rewrite its contents.
        // Otherwise, we need to create a new wl_buffer.
        if (buf->remote) {
            ww_assert(buf->width == 1 && buf->height == 1);
            manager->pool_data[buf->offset] = argb;
        } else {
            if (manager->pool_ptr == manager->pool_size) {
                expand_shm(manager);
            }

            buf->on_heap = false;

            buf->offset = manager->pool_ptr++;
            buf->data = argb;
            buf->width = 1;
            buf->height = 1;
            buf->stride = sizeof(uint32_t);
            buf->format = WL_SHM_FORMAT_ARGB8888;

            manager->pool_data[buf->offset] = argb;

            buf->remote = wl_shm_pool_create_buffer(manager->shm_pool, buf->offset, buf->width,
                                                    buf->height, buf->stride, buf->format);
            wl_buffer_set_user_data(buf->remote, buf);
        }

        buf->rc++;
        return buf->remote;
    }

    LOG(LOG_ERROR, "failed to find space for a color buffer (%" PRIu32 ")", argb);
    ww_unreachable();
}
