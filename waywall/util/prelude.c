#include "util/prelude.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

[[noreturn]] void
util_panic(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");

    fflush(stderr);
    exit(EXIT_FAILURE);
}
