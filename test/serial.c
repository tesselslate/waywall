#include "server/serial.h"
#include "util.h"

int
main() {
    struct serial_ring ring = {0};

    for (size_t i = 0; i < STATIC_ARRLEN(ring.data); i++) {
        serial_ring_push(&ring, i);
    }
    ww_assert(serial_ring_push(&ring, 64) != 0);
    for (size_t i = 0; i < STATIC_ARRLEN(ring.data); i++) {
        ww_assert(serial_ring_consume(&ring, i) == 0);
    }
    ww_assert(serial_ring_consume(&ring, 0) != 0);
}
