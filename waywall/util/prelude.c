#include "util/prelude.h"
#include <stdio.h>
#include <stdlib.h>

noreturn void
util_panic(const char *file, int line, const char *msg) {
    fprintf(stderr, "[%s:%d] %s\n", file, line, msg);
    exit(EXIT_FAILURE);
}
