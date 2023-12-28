#ifndef WAYWALL_COMPOSITOR_BOX_H
#define WAYWALL_COMPOSITOR_BOX_H

#include <stdbool.h>
#include <stdint.h>

struct box {
    int32_t x, y, width, height;
};

static inline bool
box_contains(struct box *box, int32_t x, int32_t y) {
    return box->x <= x && x <= box->x + box->width && box->y <= y && y <= box->y + box->height;
}

#endif
