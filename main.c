#include "compositor.h"
#include <assert.h>
#include <wlr/util/log.h>

bool
noop() {
    printf("Test\n");
    return false;
}

void
noop2() {}

int
main() {
    wlr_log_init(WLR_DEBUG, NULL);
    struct compositor_vtable vtable = {noop, noop, noop2};
    struct compositor *compositor = compositor_create(vtable);
    assert(compositor);

    compositor_run(compositor);
    compositor_destroy(compositor);
    return 0;
}
