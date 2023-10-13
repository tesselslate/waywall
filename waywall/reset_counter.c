#include "reset_counter.h"
#include "util.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/util/log.h>

struct reset_counter {
    char *path;
    int fd;
    int count, last_written;
    bool queue_writes;
};

bool
reset_counter_change_file(struct reset_counter *counter, const char *path) {
    ww_assert(counter);
    ww_assert(path);

    if (counter->path && strcmp(counter->path, path) == 0) {
        return true;
    }

    int fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd == -1) {
        wlr_log_errno(WLR_ERROR, "failed to open reset counter file '%s'", counter->path);
        return false;
    }

    int count = 0;
    char buf[64];
    ssize_t len = read(fd, buf, STRING_LEN(buf));
    if (len == -1) {
        wlr_log_errno(WLR_ERROR, "failed to read existing reset count");
        goto cleanup;
    } else if (len != 0) {
        buf[len] = '\0';
        char *endptr;
        count = strtol(buf, &endptr, 10);
        if (endptr == buf) {
            wlr_log(WLR_ERROR, "failed to parse existing reset count ('%s')", buf);
            goto cleanup;
        }
    }

    reset_counter_commit_writes(counter);
    if (counter->fd) {
        close(counter->fd);
    }
    if (counter->path) {
        free(counter->path);
    }

    counter->fd = fd;
    counter->path = strdup(path);
    counter->count = count;
    counter->last_written = count ? count : -1; // Ensure that the 0 gets written if needed
    return true;

cleanup:
    close(fd);
    return false;
}

void
reset_counter_commit_writes(struct reset_counter *counter) {
    ww_assert(counter);

    counter->queue_writes = false;

    if (counter->last_written == counter->count) {
        return;
    }

    if (lseek(counter->fd, 0, SEEK_SET) == -1) {
        wlr_log_errno(WLR_ERROR, "reset_counter: failed to seek start of file");
        return;
    }

    char buf[64];
    int len = snprintf(buf, ARRAY_LEN(buf), "%d\n", counter->count);
    if (len < 0) {
        wlr_log(WLR_ERROR, "failed to format reset count");
        return;
    }
    ww_assert((size_t)len < ARRAY_LEN(buf));

    ssize_t n = write(counter->fd, buf, len);
    if (n != len) {
        wlr_log_errno(WLR_ERROR, "failed to write reset count");
    }

    counter->last_written = counter->count;
}

void
reset_counter_destroy(struct reset_counter *counter) {
    ww_assert(counter);

    reset_counter_commit_writes(counter);
    close(counter->fd);
    free(counter->path);
    free(counter);
}

struct reset_counter *
reset_counter_from_file(const char *path) {
    ww_assert(path);

    struct reset_counter *counter = calloc(1, sizeof(struct reset_counter));
    ww_assert(counter);
    if (!reset_counter_change_file(counter, path)) {
        free(counter);
        return NULL;
    }

    wlr_log(WLR_INFO, "created reset counter (starting count: %d)", counter->count);
    return counter;
}

int
reset_counter_get_count(struct reset_counter *counter) {
    ww_assert(counter);

    return counter->count;
}

int
reset_counter_increment(struct reset_counter *counter) {
    ww_assert(counter);

    counter->count++;

    if (!counter->queue_writes) {
        reset_counter_commit_writes(counter);
    }
    return counter->count;
}

void
reset_counter_queue_writes(struct reset_counter *counter) {
    ww_assert(counter);

    counter->queue_writes = true;
}
