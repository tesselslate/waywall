#include "util.h"
#include <stdio.h>
#include <stdlib.h>

void
_ww_assert(const char *file, const int line, const char *expr, bool value) {
    if (!value) {
        fprintf(stderr, "[%s:%d] assert failed: '%s'\n", file, line, expr);
        exit(1);
    }
}
