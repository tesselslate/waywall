#include "counter.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int
set_file(struct counter *counter, const char *path) {
    static_assert(sizeof(long long) == sizeof(int64_t));

    char *path_dup = strdup(path);
    check_alloc(path_dup);

    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd == -1) {
        ww_log_errno(LOG_ERROR, "failed to open '%s'", path);
        goto fail_open;
    }

    char buf[64];
    ssize_t n = read(fd, buf, STATIC_ARRLEN(buf) - 1);
    int64_t count = 0;
    if (n == -1) {
        ww_log_errno(LOG_ERROR, "failed to read count from '%s'", path);
        goto fail_read;
    } else if (n > 0) {
        buf[n] = '\0';

        errno = 0;
        count = strtoll(buf, NULL, 10);
        if (errno != 0) {
            ww_log_errno(LOG_ERROR, "failed to parse reset count '%s' from '%s'", buf, path);
            goto fail_parse;
        }
    }

    ww_log(LOG_INFO, "opened file '%s' with count %" PRIi64, path, count);

    counter->fd = fd;
    counter->path = path_dup;
    counter->count = count;

    // If the new count file has no content, then we need to write a 0 when shutting down.
    counter->written = count > 0 ? count : -1;
    return 0;

fail_parse:
fail_read:
    close(fd);

fail_open:
    free(path_dup);
    return 1;
}

struct counter *
counter_create(const char *path) {
    struct counter *counter = zalloc(1, sizeof(*counter));

    counter->fd = -1;
    if (set_file(counter, path) != 0) {
        free(counter);
        return NULL;
    }

    return counter;
}

void
counter_destroy(struct counter *counter) {
    ww_log(LOG_INFO, "writing %" PRIi64 " to '%s'", counter->count, counter->path);
    counter_commit(counter);
    close(counter->fd);
    free(counter->path);
    free(counter);
}

void
counter_commit(struct counter *counter) {
    if (counter->written == counter->count) {
        return;
    }

    if (lseek(counter->fd, 0, SEEK_SET) == -1) {
        ww_log_errno(LOG_ERROR, "failed to seek to start of count file");
        return;
    }

    char buf[64];
    int len = snprintf(buf, STATIC_ARRLEN(buf), "%" PRIi64, counter->count);
    ww_assert(len > 0 && len < (int)STATIC_ARRLEN(buf));

    if (write(counter->fd, buf, len) != len) {
        ww_log_errno(LOG_ERROR, "failed to write reset count (%" PRIi64 ")", counter->count);
        return;
    }

    counter->written = counter->count;
}

uint64_t
counter_increment(struct counter *counter) {
    counter->count++;
    counter_commit(counter);
    return counter->count;
}
