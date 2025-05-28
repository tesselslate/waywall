#include "server/remote_buffer.h"
#include "server/backend.h"
#include "server/server.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include "util/syscall.h"
#include <linux/memfd.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <stdio.h>
#include <spng.h>

#define COLOR_POOL_SIZE 64
#define POOL_INITIAL_SIZE 16384

struct slot {
    size_t offset;
    ssize_t rc;
};

struct color_pool {
    struct wl_list link; // remote_buffer_manager.color_pools

    size_t offset;
    struct slot slots[COLOR_POOL_SIZE];
    uint32_t colors[COLOR_POOL_SIZE];
};

struct color_result {
    struct {
        struct color_pool *pool;
        size_t index;
    } empty, equal;
};

struct image_buffer {
    struct wl_list link; // remote_buffer_manager.image_buffers
    
    struct slot slot;  // Contains offset and rc
    int32_t width, height;
};

static inline void
assign_rgba(char *dst, const uint8_t src[static 4]) {
    dst[0] = src[2];
    dst[1] = src[1];
    dst[2] = src[0];
    dst[3] = src[3];
}

static int
expand(struct remote_buffer_manager *manager, size_t min) {
    ww_assert(min < SIZE_MAX / 2);

    if (min < manager->size) {
        return 0;
    }

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

    int ret = munmap(manager->data, prev);
    if (ret == -1) {
        ww_log_errno(LOG_ERROR, "failed to unmap old shm pool");
        ww_panic("unmapping old shm pool failed");
    }
    manager->data = new_data;

    wl_shm_pool_resize(manager->pool, manager->size);

    return 0;

fail_mmap:
fail_truncate:
    manager->size = prev;
    return 1;
}

static struct color_result
find_color_slot(struct remote_buffer_manager *manager, uint32_t rgba) {
    struct color_result result = {0};

    struct color_pool *pool;
    wl_list_for_each (pool, &manager->color_pools, link) {
        for (size_t i = 0; i < COLOR_POOL_SIZE; i++) {
            bool unused = (pool->slots[i].rc == 0);

            if (pool->colors[i] == rgba) {
                result.equal.pool = pool;
                result.equal.index = i;

                return result;
            }

            if (unused) {
                result.empty.pool = pool;
                result.empty.index = i;
                continue;
            }
        }
    }

    return result;
}

static inline struct wl_buffer *
make_color_buffer(struct remote_buffer_manager *manager, struct slot *slot) {
    struct wl_buffer *buffer =
        wl_shm_pool_create_buffer(manager->pool, slot->offset, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
    check_alloc(buffer);
    wl_buffer_set_user_data(buffer, slot);
    return buffer;
}

static struct wl_buffer *
make_image_buffer(struct remote_buffer_manager *manager, struct image_buffer *img) {
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(manager->pool, img->slot.offset, 
                                                        img->width, img->height, 
                                                        img->width * 4, WL_SHM_FORMAT_ARGB8888);
    check_alloc(buffer);
    wl_buffer_set_user_data(buffer, &img->slot);
    return buffer;
}

static uint8_t *
load_png_from_file(const char *path, int32_t *width, int32_t *height) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        ww_log_errno(LOG_ERROR, "failed to open PNG file %s", path);
        return NULL;
    }

    spng_ctx *ctx = spng_ctx_new(0);
    if (!ctx) {
        ww_log(LOG_ERROR, "failed to create spng context");
        fclose(file);
        return NULL;
    }

    uint8_t *image_data = NULL;
    int ret = spng_set_png_file(ctx, file);
    if (ret) {
        ww_log(LOG_ERROR, "failed to set PNG file: %s", spng_strerror(ret));
        goto fail_png;
    }

    struct spng_ihdr ihdr;
    ret = spng_get_ihdr(ctx, &ihdr);
    if (ret) {
        ww_log(LOG_ERROR, "failed to get PNG header: %s", spng_strerror(ret));
        goto fail_png;
    }

    *width = ihdr.width;
    *height = ihdr.height;

    size_t image_size;
    ret = spng_decoded_image_size(ctx, SPNG_FMT_RGBA8, &image_size);
    if (ret) {
        ww_log(LOG_ERROR, "failed to get decoded image size: %s", spng_strerror(ret));
        goto fail_png;
    }

    image_data = malloc(image_size);
    if (!image_data) {
        ww_log(LOG_ERROR, "failed to allocate memory for PNG data");
        goto fail_png;
    }

    ret = spng_decode_image(ctx, image_data, image_size, SPNG_FMT_RGBA8, 0);
    if (ret) {
        ww_log(LOG_ERROR, "failed to decode PNG image: %s", spng_strerror(ret));
        free(image_data);
        image_data = NULL;
    }

fail_png:
    spng_ctx_free(ctx);
    fclose(file);
    return image_data;
}

static void
convert_rgba_to_argb(uint8_t *data, size_t pixel_count) {
    for (size_t i = 0; i < pixel_count; i++) {
        uint8_t r = data[i * 4 + 0];
        uint8_t g = data[i * 4 + 1];
        uint8_t b = data[i * 4 + 2];
        uint8_t a = data[i * 4 + 3];
        
        data[i * 4 + 0] = b;
        data[i * 4 + 1] = g;
        data[i * 4 + 2] = r;
        data[i * 4 + 3] = a;
    }
}

struct remote_buffer_manager *
remote_buffer_manager_create(struct server *server) {
    // We only use RGBA8.
    uint32_t *format;
    bool ok = false;
    wl_array_for_each(format, &server->backend->shm_formats) {
        if (*format == WL_SHM_FORMAT_ARGB8888) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        ww_log(LOG_ERROR, "RGBA8 is not a supported SHM format");
        return NULL;
    }

    struct remote_buffer_manager *manager = zalloc(1, sizeof(*manager));

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

    manager->pool = wl_shm_create_pool(server->backend->shm, manager->fd, manager->size);
    check_alloc(manager->pool);

    wl_list_init(&manager->color_pools);
    wl_list_init(&manager->image_buffers);

    return manager;

fail_mmap:
fail_truncate:
    close(manager->fd);

fail_memfd:
    free(manager);
    return NULL;
}

struct wl_buffer *
remote_buffer_manager_color(struct remote_buffer_manager *manager, const uint8_t rgba[static 4]) {
    uint32_t rgba_u32 = (uint32_t)rgba[0] | ((uint32_t)rgba[1] << 8) | ((uint32_t)rgba[2] << 16) |
                        ((uint32_t)rgba[3] << 24);

    struct color_result found_slots = find_color_slot(manager, rgba_u32);
    if (found_slots.equal.pool) {
        struct color_pool *pool = found_slots.equal.pool;
        size_t idx = found_slots.equal.index;

        pool->slots[idx].rc++;
        return make_color_buffer(manager, &pool->slots[idx]);
    } else if (found_slots.empty.pool) {
        struct color_pool *pool = found_slots.empty.pool;
        size_t idx = found_slots.empty.index;

        pool->slots[idx].rc = 1;
        pool->colors[idx] = rgba_u32;
        pool->slots[idx].offset = pool->offset + idx * 4;

        assign_rgba(manager->data + pool->slots[idx].offset, rgba);
        return make_color_buffer(manager, &pool->slots[idx]);
    } else {
        struct color_pool *pool = zalloc(1, sizeof(*pool));

        pool->offset = manager->ptr;
        size_t new_ptr = manager->ptr + COLOR_POOL_SIZE * 4;
        if (expand(manager, new_ptr) != 0) {
            free(pool);
            return NULL;
        }
        manager->ptr = new_ptr;

        wl_list_insert(&manager->color_pools, &pool->link);

        // Use an empty slot from the newly created pool.
        pool->slots[0].rc = 1;
        pool->colors[0] = rgba_u32;
        pool->slots[0].offset = pool->offset;

        assign_rgba(manager->data + pool->slots[0].offset, rgba);
        return make_color_buffer(manager, &pool->slots[0]);
    }
}

struct wl_buffer *
remote_buffer_manager_png(struct remote_buffer_manager *manager, const char *png_path) {
    int32_t width, height;
    uint8_t *png_data = load_png_from_file(png_path, &width, &height);
    if (!png_data) {
        return NULL;
    }

    convert_rgba_to_argb(png_data, width * height);

    size_t image_size = width * height * 4;
    size_t new_ptr = manager->ptr + image_size;
    
    if (expand(manager, new_ptr) != 0) {
        free(png_data);
        return NULL;
    }

    struct image_buffer *img = zalloc(1, sizeof(*img));
    img->slot.offset = manager->ptr;
    img->width = width;
    img->height = height;
    img->slot.rc = 1;

    memcpy(manager->data + img->slot.offset, png_data, image_size);
    free(png_data);

    manager->ptr = new_ptr;
    wl_list_insert(&manager->image_buffers, &img->link);

    return make_image_buffer(manager, img);
}

void
remote_buffer_manager_destroy(struct remote_buffer_manager *manager) {
    struct color_pool *pool, *tmp;
    wl_list_for_each_safe (pool, tmp, &manager->color_pools, link) {
        for (size_t i = 0; i < COLOR_POOL_SIZE; i++) {
            if (pool->slots[i].rc > 0) {
                ww_panic("remote color buffer still in use");
            }
        }

        wl_list_remove(&pool->link);
        free(pool);
    }

    struct image_buffer *img, *img_tmp;
    wl_list_for_each_safe (img, img_tmp, &manager->image_buffers, link) {
        if (img->slot.rc > 0) {
            ww_panic("remote image buffer still in use");
        }
        wl_list_remove(&img->link);
        free(img);
    }

    wl_shm_pool_destroy(manager->pool);
    ww_assert(munmap(manager->data, manager->size) == 0);
    close(manager->fd);
    free(manager);
}

void
remote_buffer_deref(struct wl_buffer *buffer) {
    struct slot *slot = wl_buffer_get_user_data(buffer);
    ww_assert(slot);
    ww_assert(slot->rc > 0);

    slot->rc--;
    wl_buffer_destroy(buffer);
}
