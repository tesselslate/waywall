#include "server/xserver.h"
#include "server/server.h"
#include "server/xwayland.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include "util/syscall.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>

/*
 * This code is partially my own making, but was largely only possible to write after combing
 * through other pre-existing implementations of Xwayland support. The licenses of codebases I
 * have referred to and used code from are included below.
 *
 * ==== weston
 *
 *  Copyright © 2008-2012 Kristian Høgsberg
 *  Copyright © 2010-2012 Intel Corporation
 *  Copyright © 2010-2011 Benjamin Franzke
 *  Copyright © 2011-2012 Collabora, Ltd.
 *  Copyright © 2010 Red Hat <mjg@redhat.com>
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a
 *  copy of this software and associated documentation files (the "Software"),
 *  to deal in the Software without restriction, including without limitation
 *  the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *  and/or sell copies of the Software, and to permit persons to whom the
 *  Software is furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice (including the next
 *  paragraph) shall be included in all copies or substantial portions of the
 *  Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 *  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *  DEALINGS IN THE SOFTWARE.
 *
 * ==== wlroots
 *
 *  Copyright (c) 2017, 2018 Drew DeVault
 *  Copyright (c) 2014 Jari Vetoniemi
 *  Copyright (c) 2023 The wlroots contributors
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy of
 *  this software and associated documentation files (the "Software"), to deal in
 *  the Software without restriction, including without limitation the rights to
 *  use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 *  of the Software, and to permit persons to whom the Software is furnished to do
 *  so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 */

static constexpr char X11_LOCK_FMT[] = "/tmp/.X%d-lock";
static constexpr char X11_SOCKET_DIR[] = "/tmp/.X11-unix";
static constexpr char X11_SOCKET_FMT[] = "/tmp/.X11-unix/X%d";

static int xserver_start(struct xserver *srv);

static void
on_client_destroy(struct wl_listener *listener, void *data) {
    struct xserver *srv = wl_container_of(listener, srv, on_client_destroy);

    wl_list_remove(&srv->on_client_destroy.link);
    srv->client = NULL;

    ww_log(LOG_INFO, "Xwayland dropped wayland connection");
}

static void
handle_idle(void *data) {
    struct xserver *srv = data;
    srv->src_idle = NULL;
    xserver_start(srv);
}

static int
handle_pidfd(int32_t fd, uint32_t mask, void *data) {
    struct xserver *srv = data;

    if (waitpid(srv->pid, NULL, 0) != srv->pid) {
        ww_log_errno(LOG_ERROR, "failed to waitpid on Xwayland");
    }

    wl_event_source_remove(srv->src_pidfd);
    srv->src_pidfd = NULL;

    ww_log(LOG_INFO, "Xwayland process died");
    return 0;
}

static int
handle_xserver_ready(int32_t fd, uint32_t mask, void *data) {
    struct xserver *srv = data;

    if (mask & WL_EVENT_READABLE) {
        char buf[64] = {0};
        ssize_t n = read(fd, buf, STATIC_ARRLEN(buf));
        if (n == -1 && errno != EINTR) {
            ww_log_errno(LOG_ERROR, "failed to read from xwayland displayfd");
            mask = 0;
        } else if (n <= 0 || buf[n - 1] != '\n') {
            return 1;
        }
    }

    if (!(mask & WL_EVENT_READABLE)) {
        ww_assert(mask & WL_EVENT_HANGUP);
        ww_log(LOG_ERROR, "display pipe closed (xwayland startup failed)");
        goto fail;
    }

    wl_event_source_remove(srv->src_pipe);
    srv->src_pipe = NULL;

    wl_signal_emit_mutable(&srv->events.ready, NULL);
    return 0;

fail:
    wl_event_source_remove(srv->src_pipe);
    srv->src_pipe = NULL;

    close(fd);
    return 0;
}

static int
safe_close(int fd) {
    return (fd >= 0) ? close(fd) : 0;
}

static int
set_cloexec(int fd, bool cloexec) {
    int flags = fcntl(fd, F_GETFD);
    if (flags == -1) {
        ww_log_errno(LOG_ERROR, "fcntl(%d, F_GETFD) failed", fd);
        return 1;
    }

    flags = cloexec ? (flags | FD_CLOEXEC) : (flags & ~FD_CLOEXEC);
    if (fcntl(fd, F_SETFD, flags) != 0) {
        ww_log_errno(LOG_ERROR, "fcntl(%d, F_SETFD, %x) failed", fd, flags);
        return 1;
    }

    return 0;
}

static int
open_socket(struct sockaddr_un *addr, size_t path_size) {
    socklen_t size = offsetof(struct sockaddr_un, sun_path) + path_size + 1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        ww_log_errno(LOG_ERROR, "failed to create socket %c%s",
                     addr->sun_path[0] ? addr->sun_path[0] : '@', addr->sun_path + 1);
        return -1;
    }

    if (addr->sun_path[0]) {
        unlink(addr->sun_path);
    }

    if (bind(fd, (struct sockaddr *)addr, size) == -1) {
        ww_log_errno(LOG_ERROR, "failed to bind socket %c%s",
                     addr->sun_path[0] ? addr->sun_path[0] : '@', addr->sun_path + 1);
        close(fd);
        return -1;
    }

    if (listen(fd, 1) == -1) {
        ww_log_errno(LOG_ERROR, "failed to listen to socket %c%s",
                     addr->sun_path[0] ? addr->sun_path[0] : '@', addr->sun_path + 1);
        close(fd);
        return -1;
    }

    return fd;
}

static int
open_sockets(int display, int lock_fd, int x_sockets[static 2]) {
    if (mkdir(X11_SOCKET_DIR, 0755) == 0) {
        ww_log(LOG_WARN, "created X11 socket directory");
    } else if (errno != EEXIST) {
        ww_log_errno(LOG_ERROR, "could not create X11 socket directory");
        return -1;
    } else {
        // There are some potential security concerns when not checking the X11 socket directory
        // (i.e. other users may be able to mess with our X11 sockets) but let's be real it doesn't
        // really matter, we're playing Minecraft.
        ww_log(LOG_INFO, "using existing X11 socket directory");
    }

    struct sockaddr_un addr = {.sun_family = AF_UNIX};

    // Open the abstract X11 socket.
    addr.sun_path[0] = 0;
    size_t path_size =
        snprintf(addr.sun_path + 1, STATIC_ARRLEN(addr.sun_path) - 1, X11_SOCKET_FMT, display);
    x_sockets[0] = open_socket(&addr, path_size);
    if (x_sockets[0] == -1) {
        return -1;
    }

    // Open the non-abstract X11 socket.
    path_size = snprintf(addr.sun_path, STATIC_ARRLEN(addr.sun_path), X11_SOCKET_FMT, display);
    x_sockets[1] = open_socket(&addr, path_size);
    if (x_sockets[1] == -1) {
        close(x_sockets[0]);
        return -1;
    }

    char pidstr[12];
    snprintf(pidstr, STATIC_ARRLEN(pidstr), "%10d", getpid());
    if (write(lock_fd, pidstr, STATIC_ARRLEN(pidstr)) != STATIC_ARRLEN(pidstr)) {
        ww_log(LOG_ERROR, "failed to write X11 lock file");
        close(x_sockets[1]);
        close(x_sockets[0]);
        return -1;
    }

    return 0;
}

static int
get_display(int x_sockets[static 2]) {
    for (int display = 0; display <= 32; display++) {
        char lock_name[64];
        snprintf(lock_name, sizeof(lock_name), X11_LOCK_FMT, display);

        // Attempt to acquire the lock file for this display.
        int lock_fd = open(lock_name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444);
        if (lock_fd >= 0) {
            if (open_sockets(display, lock_fd, x_sockets) == 0) {
                close(lock_fd);
                return display;
            } else {
                unlink(lock_name);
                close(lock_fd);
                continue;
            }
        }

        // If the lock file already exists, check to see if the owning process is still alive.
        lock_fd = open(lock_name, O_RDONLY | O_CLOEXEC);
        if (lock_fd == -1) {
            ww_log_errno(LOG_ERROR, "skipped %s: failed to open for reading", lock_name);
            continue;
        }

        char pidstr[12] = {0};
        ssize_t n = read(lock_fd, pidstr, STATIC_STRLEN(pidstr));
        close(lock_fd);

        if (n != STATIC_STRLEN(pidstr)) {
            ww_log(LOG_INFO, "skipped %s: length %zu", lock_name, n);
            continue;
        }

        long pid = strtol(pidstr, NULL, 10);
        if (pid < 0 || pid > INT32_MAX) {
            ww_log(LOG_INFO, "skipped %s: invalid pid %ld", lock_name, pid);
            continue;
        }

        errno = 0;
        if (kill((pid_t)pid, 0) == 0 || errno != ESRCH) {
            ww_log(LOG_INFO, "skipped %s: process alive (%ld)", lock_name, pid);
            continue;
        }

        // The process is no longer alive. Try to take the display.
        if (unlink(lock_name) != 0) {
            ww_log_errno(LOG_ERROR, "skipped %s: failed to unlink", lock_name);
            continue;
        }

        lock_fd = open(lock_name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444);
        if (lock_fd >= 0) {
            if (open_sockets(display, lock_fd, x_sockets) == 0) {
                close(lock_fd);
                return display;
            } else {
                unlink(lock_name);
                close(lock_fd);
                continue;
            }
        }
    }

    return -1;
}

static void
unlink_display(int display) {
    char path[64];

    snprintf(path, STATIC_ARRLEN(path), X11_SOCKET_FMT, display);
    unlink(path);

    snprintf(path, STATIC_ARRLEN(path), X11_LOCK_FMT, display);
    unlink(path);
}

static void
xserver_exec(struct xserver *srv, int notify_fd, int log_fd) {
    // This function should only ever be run in the context of the child process created from
    // `xserver_start`.

    // Unset CLOEXEC on the file descriptors which will be owned by the X server.
    const int fds[] = {
        srv->fd_xwm[1],
        srv->fd_wl[1],
        srv->x_sockets[0],
        srv->x_sockets[1],
    };

    for (size_t i = 0; i < STATIC_ARRLEN(fds); i++) {
        if (set_cloexec(fds[i], false) != 0) {
            return;
        }
    }

    // Determine the Xwayland binary to use.
    char *xwl_path = getenv("WAYWALL_XWAYLAND_BINARY");
    if (!xwl_path) {
        xwl_path = "Xwayland";
    }

    // Build the command to pass to execvp.
    char *argv[64];
    size_t i = 0;

    char listenfd0[16], listenfd1[16], displayfd[16], wmfd[16];
    snprintf(listenfd0, STATIC_ARRLEN(listenfd0), "%d", srv->x_sockets[0]);
    snprintf(listenfd1, STATIC_ARRLEN(listenfd1), "%d", srv->x_sockets[1]);
    snprintf(displayfd, STATIC_ARRLEN(displayfd), "%d", notify_fd);
    snprintf(wmfd, STATIC_ARRLEN(wmfd), "%d", srv->fd_xwm[1]);

    argv[i++] = xwl_path;
    argv[i++] = srv->display_name;
    argv[i++] = "-rootless"; // run in rootless mode
    argv[i++] = "-core";     // make core dumps
    argv[i++] = "-noreset";  // do not reset when the last client disconnects

    argv[i++] = "-listenfd";
    argv[i++] = listenfd0;
    argv[i++] = "-listenfd";
    argv[i++] = listenfd1;

    argv[i++] = "-displayfd";
    argv[i++] = displayfd;

    argv[i++] = "-wm";
    argv[i++] = wmfd;

    argv[i++] = NULL;
    ww_assert(i < STATIC_ARRLEN(argv));

    // Set WAYLAND_SOCKET so that the X server will connect correctly.
    char wayland_socket[16];
    snprintf(wayland_socket, STATIC_ARRLEN(wayland_socket), "%d", srv->fd_wl[1]);
    setenv("WAYLAND_SOCKET", wayland_socket, true);

    // Set stdout and stderr to go to the Xwayland log file. Keep a CLOEXEC backup of stderr in case
    // we need to print an error.
    int stderr_backup = dup(STDERR_FILENO);
    if (stderr_backup == -1) {
        ww_log_errno(LOG_ERROR, "failed to backup Xwayland stderr fd");
    } else {
        set_cloexec(stderr_backup, true);
    }

    if (dup2(log_fd, STDOUT_FILENO) == -1) {
        ww_log_errno(LOG_ERROR, "failed to dup log_fd to stdout");
    }
    if (dup2(log_fd, STDERR_FILENO) == -1) {
        ww_log_errno(LOG_ERROR, "failed to dup log_fd to stderr");
    }

    close(log_fd);

    ww_assert(close(STDIN_FILENO) == 0);

    execvp(argv[0], argv);

    // Restore stderr to print the error message.
    if (stderr_backup != -1) {
        dup2(stderr_backup, STDERR_FILENO);
        ww_log_errno(LOG_ERROR, "failed to exec Xwayland");
        close(log_fd);
    }
}

static int
xserver_start(struct xserver *srv) {
    // Create the Wayland client for the Xwayland connection.
    srv->client = wl_client_create(srv->wl_display, srv->fd_wl[0]);
    if (!srv->client) {
        ww_log_errno(LOG_ERROR, "failed to create wayland client for xserver");
        return 1;
    }

    srv->on_client_destroy.notify = on_client_destroy;
    wl_client_add_destroy_listener(srv->client, &srv->on_client_destroy);

    // Create the pipe for knowing when the X server is ready.
    int notify_fd[2];
    if (pipe(notify_fd) != 0) {
        ww_log_errno(LOG_ERROR, "failed to create readiness pipe for xserver");
        return 1;
    }
    if (set_cloexec(notify_fd[0], true) != 0) {
        goto fail_cloexec;
    }

    // Create the readiness notification.
    struct wl_event_loop *loop = wl_display_get_event_loop(srv->wl_display);
    srv->src_pipe =
        wl_event_loop_add_fd(loop, notify_fd[0], WL_EVENT_READABLE, handle_xserver_ready, srv);

    // Create the log file for Xwayland.
    char logname[32] = {0};
    ssize_t n = snprintf(logname, STATIC_ARRLEN(logname), "xwayland-%jd", (intmax_t)getpid());
    ww_assert(n < (ssize_t)STATIC_ARRLEN(logname));

    int log_fd = util_log_create_file(logname, false);
    if (log_fd == -1) {
        goto fail_log;
    }

    // Spawn the child process.
    srv->pid = fork();
    if (srv->pid == 0) {
        // Child process
        xserver_exec(srv, notify_fd[1], log_fd);
        exit(EXIT_FAILURE);
    } else if (srv->pid == -1) {
        // Parent process (error)
        ww_log_errno(LOG_ERROR, "failed to fork xwayland");
        goto fail_fork;
    }

    // The Xwayland process will own the log file descriptor, the X11 socket file descriptors,
    // the other halves of the Wayland/XWM socket pairs, and the other half of the displayfd pipe.
    // Close them since they are no longer needed.
    close(log_fd);
    close(srv->x_sockets[0]);
    close(srv->x_sockets[1]);
    close(srv->fd_wl[1]);
    close(srv->fd_xwm[1]);
    close(notify_fd[1]);

    log_fd = -1;
    srv->x_sockets[0] = -1;
    srv->x_sockets[1] = -1;
    srv->fd_wl[1] = -1;
    srv->fd_xwm[1] = -1;
    notify_fd[1] = -1;

    // Open a pidfd for the Xwayland process so it can be killed when waywall shuts down.
    srv->pidfd = pidfd_open(srv->pid, 0);
    if (srv->pidfd == -1) {
        ww_log_errno(LOG_ERROR, "failed to open pidfd");

        if (kill(srv->pid, SIGKILL) == -1) {
            ww_log_errno(LOG_ERROR, "failed to kill xwayland");
        }

        goto fail_pidfd;
    }

    srv->src_pidfd = wl_event_loop_add_fd(loop, srv->pidfd, WL_EVENT_READABLE, handle_pidfd, srv);
    check_alloc(srv->src_pidfd);

    ww_log(LOG_INFO, "using X11 display :%d", srv->display);

    return 0;

fail_pidfd:
fail_fork:
    safe_close(srv->x_sockets[0]);
    safe_close(srv->x_sockets[1]);
    srv->x_sockets[0] = srv->x_sockets[1] = -1;

    unlink_display(srv->display);
    safe_close(log_fd);

fail_log:
    wl_event_source_remove(srv->src_pipe);
    srv->src_pipe = NULL;

fail_cloexec:
    safe_close(notify_fd[0]);
    safe_close(notify_fd[1]);
    return 1;
}

struct xserver *
xserver_create(struct server_xwayland *xwl) {
    struct xserver *srv = zalloc(1, sizeof(*srv));
    srv->wl_display = xwl->server->display;

    srv->x_sockets[0] = srv->x_sockets[1] = -1;
    srv->fd_xwm[0] = srv->fd_xwm[1] = -1;
    srv->fd_wl[0] = srv->fd_wl[1] = -1;

    srv->pidfd = -1;

    // Create socket pairs for the Wayland connection and XWM connection.
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, srv->fd_wl) != 0) {
        ww_log_errno(LOG_ERROR, "failed to create wayland socket pair");
        goto fail;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, srv->fd_xwm) != 0) {
        ww_log_errno(LOG_ERROR, "failed to create xwm socket pair");
        goto fail;
    }

    // Acquire and lock an X11 display immediately so that the DISPLAY environment variable can be
    // set before any child processes are launched.
    srv->display = get_display(srv->x_sockets);
    if (srv->display == -1) {
        ww_log(LOG_ERROR, "failed to acquire an X11 display");
        goto fail;
    }
    snprintf(srv->display_name, STATIC_ARRLEN(srv->display_name), ":%d", srv->display);
    setenv("DISPLAY", srv->display_name, true);

    // Register an idle source to start the Xwayland server.
    srv->src_idle =
        wl_event_loop_add_idle(wl_display_get_event_loop(srv->wl_display), handle_idle, srv);
    check_alloc(srv->src_idle);

    wl_signal_init(&srv->events.ready);

    return srv;

fail:
    xserver_destroy(srv);
    return NULL;
}

void
xserver_destroy(struct xserver *srv) {
    if (srv->client) {
        wl_list_remove(&srv->on_client_destroy.link);
        wl_client_destroy(srv->client);
    }

    if (srv->src_idle) {
        wl_event_source_remove(srv->src_idle);
    }
    if (srv->src_pidfd) {
        wl_event_source_remove(srv->src_pidfd);
    }
    if (srv->src_pipe) {
        wl_event_source_remove(srv->src_pipe);
    }

    safe_close(srv->x_sockets[0]);
    safe_close(srv->x_sockets[1]);
    safe_close(srv->fd_xwm[0]);
    safe_close(srv->fd_xwm[1]);
    safe_close(srv->fd_wl[0]);
    safe_close(srv->fd_wl[1]);

    if (srv->pidfd >= 0) {
        if (pidfd_send_signal(srv->pidfd, SIGKILL, NULL, 0) != 0) {
            ww_log_errno(LOG_ERROR, "failed to send SIGKILL to xserver");
        }
        close(srv->pidfd);
    }

    unlink_display(srv->display);

    free(srv);
}
