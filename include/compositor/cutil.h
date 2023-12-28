#ifndef WAYWALL_COMPOSITOR_CUTIL_H
#define WAYWALL_COMPOSITOR_CUTIL_H

#include "util.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARRAY_SIZE 64
#define RINGBUF_SIZE 64

/*
 *  box is a rectangle.
 */
struct box {
    int32_t x, y, width, height;
};

/*
 *  ringbuf provides a simple, fixed-size circular buffer for holding event serials.
 */
struct ringbuf {
    uint32_t data[RINGBUF_SIZE];
    size_t head, tail, count;
};

/*
 *  u32_array is a fixed size array of uint32_t, used for storing data like held keys or buttons.
 */
struct u32_array {
    uint32_t data[ARRAY_SIZE];
    size_t count;
};

/*
 *  Returns whether or not the box contains the point.
 */
static inline bool
box_contains(struct box *box, int32_t x, int32_t y) {
    return box->x <= x && x <= box->x + box->width && box->y <= y && y <= box->y + box->height;
}

/*
 *  Returns the element at the given index of the ring buffer.
 */
static inline uint32_t
ringbuf_at(struct ringbuf *ringbuf, size_t index) {
    ww_assert(index < ringbuf->count);

    return ringbuf->data[(ringbuf->head + index) % RINGBUF_SIZE];
}

/*
 *  Pushes the given element to the tail of the ring buffer.
 */
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

/*
 *  Pops the first `num` elements from the head of the ring buffer, shifting it forward.
 */
static inline void
ringbuf_shift(struct ringbuf *ringbuf, size_t num) {
    ww_assert(ringbuf->count >= num);

    ringbuf->count -= num;

    ringbuf->head += num;
    ringbuf->head %= RINGBUF_SIZE;
}

/*
 *  Pushes a uint32_t onto the array.
 */
static inline void
u32_array_push(struct u32_array *arr, uint32_t datum) {
    ww_assert(arr->count < ARRAY_SIZE);
    arr->data[arr->count++] = datum;
}

/*
 *  Removes the element at the given index.
 */
static inline void
u32_array_remove(struct u32_array *arr, size_t i) {
    memmove(arr->data + i, arr->data + i + 1, arr->count - i - 1);
    arr->count--;
}

#endif
