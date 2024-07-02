#include "util/zip.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static const uint32_t MAGIC_EOCD = 0x06054B50;
static const uint32_t MAGIC_CD = 0x02014B50;

#define SIZE_EOCD 22
#define SIZE_CD 46

struct zip {
    int fd;

    struct {
        char *region;
        size_t len;
    } map;

    struct zip_eocd {
        uint32_t signature;
        uint16_t disk_num;
        uint16_t disk_cd_start;
        uint16_t disk_cd_records;
        uint16_t cd_records;
        uint32_t cd_size;
        uint32_t cd_offset;
        uint16_t comment_len;
    } __attribute__((packed)) eocd;

    struct {
        size_t idx;
        size_t offset;
        char *filename;
    } iter;
};

struct zip_cd {
    uint32_t signature;
    uint16_t version_made;
    uint16_t version_extract;
    uint16_t flags;
    uint16_t compression;
    uint16_t modification_time;
    uint16_t modification_date;
    uint32_t crc_uncompressed;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t file_name_len;
    uint16_t extra_field_len;
    uint16_t file_comment_len;
    uint16_t disk_number_start;
    uint16_t internal_attributes;
    uint32_t external_attributes;
    uint32_t local_header_offset;
} __attribute__((packed));

static_assert(sizeof(struct zip_eocd) == SIZE_EOCD);
static_assert(sizeof(struct zip_cd) == SIZE_CD);

static inline uint32_t
read32_le(const char *buf) {
    uint32_t bytes[4] = {buf[0], buf[1], buf[2], buf[3]};
    return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
}

static bool
is_eocd_zip64(struct zip_eocd eocd) {
    return eocd.cd_offset == 0xFFFFFFFF && eocd.cd_size == 0xFFFFFFFF && eocd.cd_records == 0xFFFF;
}

static int
read_eocd(struct zip *zip) {
    // Locate the End of Central Directory (EOCD) structure first by reading backwards from the end
    // of the ZIP file and looking for its signature (MAGIC_EOCD).
    //
    // We assume that the first instance of the magic value is the correct one.
    ssize_t offset = zip->map.len - SIZE_EOCD;
    bool found = false;
    for (; offset >= 0; offset--) {
        uint32_t magic = read32_le(zip->map.region + offset);
        if (magic == MAGIC_EOCD) {
            found = true;
            break;
        }
    }
    if (!found) {
        ww_log(LOG_ERROR, "failed to find EOCD magic signature");
        return 1;
    }

    memcpy(&zip->eocd, zip->map.region + offset, SIZE_EOCD);

    if (is_eocd_zip64(zip->eocd)) {
        // TODO: Support ZIP64
        ww_log(LOG_ERROR, "ZIP64 file detected - unsupported");
        return 1;
    }

    // Prepare the data in zip->iter for calls to `zip_next`.
    zip->iter.offset = zip->eocd.cd_offset;

    return 0;
}

void
zip_close(struct zip *zip) {
    close(zip->fd);
    ww_assert(munmap(zip->map.region, zip->map.len) == 0);

    if (zip->iter.filename) {
        free(zip->iter.filename);
    }

    free(zip);
}

const char *
zip_next(struct zip *zip) {
    if (zip->iter.idx == zip->eocd.cd_records) {
        return NULL;
    }

    if (zip->iter.offset + SIZE_CD >= zip->map.len) {
        ww_log(LOG_ERROR, "malformed zip file - CD record extends past EOF");
        goto fail;
    }

    // Ensure we are actually reading a Central Directory record by checking for the magic
    // signature.
    uint32_t magic = read32_le(zip->map.region + zip->iter.offset);
    if (magic != MAGIC_CD) {
        ww_log(LOG_ERROR, "failed to find CD magic signature while reading zip");
        goto fail;
    }

    struct zip_cd cdr = {0};
    memcpy(&cdr, zip->map.region + zip->iter.offset, SIZE_CD);

    // Check the full size of the Central Directory record against the size of the ZIP file.
    size_t record_size = SIZE_CD + cdr.file_name_len + cdr.extra_field_len + cdr.file_comment_len;
    if (zip->iter.offset + record_size >= zip->map.len) {
        ww_log(LOG_ERROR, "malformed zip file - CD record extends past EOF");
        goto fail;
    }

    // We need to allocate storage for the file name and copy it to our own buffer because it will
    // not have a null terminator.
    if (zip->iter.filename) {
        free(zip->iter.filename);
    }
    zip->iter.filename = zalloc(cdr.file_name_len + 1, 1);
    memcpy(zip->iter.filename, zip->map.region + zip->iter.offset + SIZE_CD, cdr.file_name_len);

    // Move the iterator forward to the next Central Directory record and return the filename.
    zip->iter.offset += record_size;
    zip->iter.idx++;
    return zip->iter.filename;

fail:
    // Prevent any further calls to `zip_next` by moving the iterator to the end.
    zip->iter.idx = zip->eocd.cd_records;
    return NULL;
}

struct zip *
zip_open(const char *path) {
    struct zip *zip = zalloc(1, sizeof(*zip));

    zip->fd = open(path, O_RDONLY);
    if (zip->fd == -1) {
        ww_log_errno(LOG_ERROR, "failed to open zip at '%s'", path);
        goto fail_open;
    }

    struct stat zipstat = {0};
    if (fstat(zip->fd, &zipstat) == -1) {
        ww_log_errno(LOG_ERROR, "failed to stat zip at '%s'", path);
        goto fail_stat;
    }

    zip->map.len = zipstat.st_size;
    zip->map.region = mmap(NULL, zip->map.len, PROT_READ, MAP_SHARED, zip->fd, 0);
    if (zip->map.region == MAP_FAILED) {
        ww_log_errno(LOG_ERROR, "failed to mmap zip at '%s'", path);
        goto fail_mmap;
    }

    if (read_eocd(zip) != 0) {
        ww_log(LOG_ERROR, "failed to read zip at '%s'", path);
        goto fail_read;
    }

    return zip;

fail_read:
    ww_assert(munmap(zip->map.region, zip->map.len) == 0);

fail_mmap:
fail_stat:
    close(zip->fd);

fail_open:
    free(zip);
    return NULL;
}
