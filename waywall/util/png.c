#include "util/png.h"
#include "util/alloc.h"
#include "util/log.h"
#include <fcntl.h>
#include <spng.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct util_png
util_png_decode(const char *path, int max_size) {
    struct util_png result = {0};

    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        ww_log_errno(LOG_ERROR, "failed to open PNG");
        return result;
    }

    struct stat stat;
    if (fstat(fd, &stat) != 0) {
        ww_log_errno(LOG_ERROR, "failed to stat PNG");
        goto fail_stat;
    }

    void *buf = mmap(nullptr, stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (buf == MAP_FAILED) {
        ww_log_errno(LOG_ERROR, "failed to mmap PNG (size %ju)", (uintmax_t)stat.st_size);
        goto fail_mmap;
    }

    struct spng_ctx *ctx = spng_ctx_new(0);
    if (!ctx) {
        ww_log(LOG_ERROR, "failed to create spng context");
        goto fail_spng_ctx;
    }
    spng_set_image_limits(ctx, max_size, max_size);

    int err = spng_set_png_buffer(ctx, buf, stat.st_size);
    if (err != 0) {
        ww_log(LOG_ERROR, "failed to set PNG buffer: %s", spng_strerror(err));
        goto fail_spng_set_png_buffer;
    }

    struct spng_ihdr ihdr;
    err = spng_get_ihdr(ctx, &ihdr);
    if (err != 0) {
        ww_log(LOG_ERROR, "failed to get image header: %s", spng_strerror(err));
        goto fail_spng_get_ihdr;
    }

    err = spng_decoded_image_size(ctx, SPNG_FMT_RGBA8, &result.size);
    if (err != 0) {
        ww_log(LOG_ERROR, "failed to get size of decoded image: %s", spng_strerror(err));
        goto fail_spng_decoded_image_size;
    }

    result.data = malloc(result.size);
    check_alloc(result.data);

    err = spng_decode_image(ctx, result.data, result.size, SPNG_FMT_RGBA8, SPNG_DECODE_TRNS);
    if (err != 0) {
        ww_log(LOG_ERROR, "failed to decode image: %s", spng_strerror(err));
        goto fail_spng_decode_image;
    }

    result.width = ihdr.width;
    result.height = ihdr.height;

    spng_ctx_free(ctx);
    munmap(buf, stat.st_size);
    close(fd);

    return result;

fail_spng_decode_image:
    free(result.data);

fail_spng_decoded_image_size:
fail_spng_get_ihdr:
fail_spng_set_png_buffer:
    spng_ctx_free(ctx);

fail_spng_ctx:
    munmap(buf, stat.st_size);

fail_mmap:
fail_stat:
    close(fd);

    result.data = nullptr;
    return result;
}
