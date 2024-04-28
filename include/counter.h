#ifndef WAYWALL_COUNTER_H
#define WAYWALL_COUNTER_H

#include <stdint.h>

struct counter {
    int fd;
    char *path;

    int64_t count, written;
};

struct counter *counter_create(const char *path);
void counter_destroy(struct counter *counter);

void counter_commit(struct counter *counter);
uint64_t counter_increment(struct counter *counter);

#endif
