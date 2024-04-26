#define _DEFAULT_SOURCE

#include "util/syscall.h"
#include <sys/syscall.h>
#include <unistd.h>

int
memfd_create(const char *name, unsigned int flags) {
    return syscall(SYS_memfd_create, name, flags);
}
