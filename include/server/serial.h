#ifndef WAYWALL_SERVER_SERIAL_H
#define WAYWALL_SERVER_SERIAL_H

#include "util.h"
#include <stddef.h>
#include <stdint.h>

struct serial_ring {
    uint32_t data[64];
    size_t tail, len;
};

static int
serial_ring_consume(struct serial_ring *ring, uint32_t serial) {
    for (size_t i = 0; i < ring->len; i++) {
        uint32_t datum = ring->data[(ring->tail + i) % STATIC_ARRLEN(ring->data)];
        if (datum == serial) {
            ring->tail = (ring->tail + i + 1) % STATIC_ARRLEN(ring->data);
            ring->len = ring->len - i - 1;
            return 0;
        }
    }
    return 1;
}

static int
serial_ring_push(struct serial_ring *ring, uint32_t serial) {
    if (ring->len == STATIC_ARRLEN(ring->data)) {
        return 1;
    }
    ring->data[(ring->tail + ring->len) % STATIC_ARRLEN(ring->data)] = serial;
    ring->len++;
    return 0;
}

#endif
