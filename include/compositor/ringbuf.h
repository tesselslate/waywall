#ifndef WAYWALL_COMPOSITOR_RINGBUF_H
#define WAYWALL_COMPOSITOR_RINGBUF_H

#include "util.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define RINGBUF_SIZE 64

/*
 *  ringbuf provides a simple, fixed-size circular buffer for holding event serials.
 */
struct ringbuf {
    uint32_t data[RINGBUF_SIZE];
    size_t head, tail, count;
};

static inline uint32_t
ringbuf_at(struct ringbuf *ringbuf, size_t index) {
    ww_assert(index < ringbuf->count);

    return ringbuf->data[(ringbuf->head + index) % RINGBUF_SIZE];
}

static inline bool
ringbuf_push(struct ringbuf *ringbuf, uint32_t datum) {
    if (ringbuf->count == RINGBUF_SIZE) {
        return false;
    }

    ringbuf->data[ringbuf->tail] = datum;
    ringbuf->count++;

    ringbuf->tail++;
    ringbuf->tail %= RINGBUF_SIZE;

    return true;
}

static inline void
ringbuf_shift(struct ringbuf *ringbuf, size_t num) {
    ww_assert(ringbuf->count >= num);

    ringbuf->count -= num;

    ringbuf->head += num;
    ringbuf->head %= RINGBUF_SIZE;
}

#endif
