#ifndef WAYWALL_COMPOSITOR_BOX_H
#define WAYWALL_COMPOSITOR_BOX_H

#include <stdbool.h>
#include <stdint.h>

/*
 *  box is a rectangle.
 */
struct box {
    int32_t x, y, width, height;
};

/*
 *  Returns whether or not the box contains the point.
 */
static inline bool
box_contains(struct box *box, int32_t x, int32_t y) {
    return box->x <= x && x <= box->x + box->width && box->y <= y && y <= box->y + box->height;
}

#endif
