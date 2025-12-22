#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include "util/syscall.h"
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

// execvpe is reexposed through this function to contain _GNU_SOURCE to this file, where it's more
// obvious if something non-portable is being used. execvpe is available in both musl and glibc, so
// it should be fine for the vast majority of Linux systems.
int
util_execvpe(const char *file, char *const argv[], char *const envp[]) {
    return execvpe(file, argv, envp);
}

int
memfd_create(const char *name, unsigned int flags) {
    return syscall(SYS_memfd_create, name, flags);
}

int
pidfd_open(pid_t pid, unsigned int flags) {
    return syscall(SYS_pidfd_open, pid, flags);
}

int
pidfd_send_signal(int pidfd, int sig, siginfo_t *info, unsigned int flags) {
    return syscall(SYS_pidfd_send_signal, pidfd, sig, info, flags);
}
