#include "util/debug.h"
#include "util/log.h"
#include "util/prelude.h"
#include <stdio.h>

bool util_debug_enabled = false;
struct util_debug util_debug_data = {};

static char debug_buf[524288] = {0};
static FILE *debug_file = NULL;

bool
util_debug_init() {
    debug_file = fmemopen(debug_buf, STATIC_STRLEN(debug_buf), "w");
    if (!debug_file) {
        ww_log_errno(LOG_ERROR, "failed to open memory-backed file");
        return false;
    }

    return true;
}

const char *
util_debug_str() {
    ww_assert(debug_file);
    ww_assert(fseek(debug_file, 0, SEEK_SET) == 0);

    fprintf(debug_file, "debug enabled\n");

    ww_assert(fflush(debug_file) == 0);
    return debug_buf;
}
