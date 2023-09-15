#include "compositor.h"
#include <assert.h>
#include <wlr/util/log.h>

int
main() {
    wlr_log_init(WLR_DEBUG, NULL);
    struct compositor *compositor = compositor_create();
    assert(compositor);

    compositor_run(compositor);
    compositor_destroy(compositor);
    return 0;
}
