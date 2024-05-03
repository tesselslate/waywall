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

static int xserver_start(struct xserver *srv);

static const char X11_LOCK_FMT[] = "/tmp/.X%d-lock";
static const char X11_SOCKET_DIR[] = "/tmp/.X11-unix/";
static const char X11_SOCKET_FMT[] = "/tmp/.X11-unix/X%d";

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

    wl_event_source_remove(srv->src_pidfd);
    srv->src_pidfd = NULL;

    ww_log(LOG_INFO, "Xwayland process died");
    return 0;
}

static int
handle_xserver_ready(int32_t fd, uint32_t mask, void *data) {
    struct xserver *srv = data;

    // To be honest, I have no idea what any of this does. I took it from wlroots.
    if (mask & WL_EVENT_READABLE) {
        char buf[64];
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
        ww_log(LOG_ERROR, "xwayland startup failed");
        goto fail;
    }

    wl_event_source_remove(srv->src_pipe);
    srv->src_pipe = NULL;

    wl_signal_emit_mutable(&srv->events.ready, NULL);
    return 0;

fail:
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
ensure_socket_dir() {
    // If the directory does not exist, we can create it ourselves.
    if (mkdir(X11_SOCKET_DIR, 0777) == 0) {
        ww_log(LOG_WARN, "created X11 socket dir - unwritable by other users");
        return 0;
    } else if (errno != EEXIST) {
        ww_log_errno(LOG_ERROR, "failed to create X11 socket dir '%s'", X11_SOCKET_DIR);
        return 1;
    }

    // Ensure that the socket directory is a directory.
    struct stat stat;
    if (lstat(X11_SOCKET_DIR, &stat) != 0) {
        ww_log_errno(LOG_ERROR, "failed to stat X11 socket dir '%s'", X11_SOCKET_DIR);
        return 1;
    }
    if (!S_ISDIR(stat.st_mode)) {
        ww_log(LOG_ERROR, "X11 socket dir '%s' is not a directory", X11_SOCKET_DIR);
        return 1;
    }

    // TODO: wlroots does some additional permissions checks but I don't think other users messing
    // with our X11 sockets is a huge concern

    return 0;
}

static int
open_socket(struct sockaddr_un *addr, size_t len) {
    socklen_t size = offsetof(struct sockaddr_un, sun_path) + len + 1;
    char err_prefix = addr->sun_path[0] ? addr->sun_path[0] : '@';

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd == -1) {
        ww_log_errno(LOG_ERROR, "failed to create socket %c%s", err_prefix, addr->sun_path + 1);
        return -1;
    }

    if (addr->sun_path[0]) {
        unlink(addr->sun_path);
    }

    if (bind(fd, (struct sockaddr *)addr, size) != 0) {
        ww_log_errno(LOG_ERROR, "failed to bind socket %c%s", err_prefix, addr->sun_path + 1);
        goto fail;
    }

    if (listen(fd, 1) != 0) {
        ww_log_errno(LOG_ERROR, "failed to listen on socket %c%s", err_prefix, addr->sun_path + 1);
        goto fail;
    }

    return fd;

fail:
    close(fd);
    if (addr->sun_path[0]) {
        unlink(addr->sun_path);
    }
    return -1;
}

static int
open_x11_display(int display, int fd_x11[static 2], int lock_fd) {
    if (ensure_socket_dir() != 0) {
        return 1;
    }

    // Open a socket pair.
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_LOCAL;

    addr.sun_path[0] = 0;
    ssize_t len =
        snprintf(addr.sun_path + 1, STATIC_ARRLEN(addr.sun_path) - 1, X11_SOCKET_FMT, display);
    fd_x11[0] = open_socket(&addr, len);
    if (fd_x11[0] == -1) {
        return 1;
    }

    len = snprintf(addr.sun_path, STATIC_ARRLEN(addr.sun_path), X11_SOCKET_FMT, display);
    fd_x11[1] = open_socket(&addr, len);
    if (fd_x11[1] == -1) {
        close(fd_x11[0]);
        fd_x11[0] = -1;
        return 1;
    }

    // Write the compositor PID to the X11 lockfile.
    char pid[12] = {0};
    snprintf(pid, sizeof(pid), "%10d", getpid());
    if (write(lock_fd, pid, STATIC_STRLEN(pid)) != STATIC_STRLEN(pid)) {
        close(fd_x11[0]);
        close(fd_x11[1]);
        fd_x11[0] = fd_x11[1] = -1;

        return 1;
    }

    return 0;
}

static int
acquire_x11_display(int fd_x11[static 2]) {
    for (int display = 0; display <= 32; display++) {
        char lock_name[64] = {0};
        ssize_t n = snprintf(lock_name, STATIC_ARRLEN(lock_name), X11_LOCK_FMT, display);
        ww_assert(n <= (ssize_t)STATIC_STRLEN(lock_name));

        // If the lock file does not exist (O_EXCL), we can take it for ourselves.
        int lock_fd = open(lock_name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444);
        if (lock_fd >= 0) {
            if (open_x11_display(display, fd_x11, lock_fd) != 0) {
                unlink(lock_name);
                close(lock_fd);
                continue;
            }

            close(lock_fd);
            return display;
        }

        // If the lock file does exist, see if it is actually in use anymore. Read the process ID
        // from the lock file and then use `kill()` to check if it is still alive.
        lock_fd = open(lock_name, O_RDONLY | O_CLOEXEC);
        if (lock_fd == -1) {
            continue;
        }

        char pidbuf[12] = {0};
        if (read(lock_fd, pidbuf, STATIC_STRLEN(pidbuf)) != STATIC_STRLEN(pidbuf)) {
            close(lock_fd);
            continue;
        }
        close(lock_fd);

        char *endptr = NULL;
        long pid = strtol(pidbuf, &endptr, 10);
        if (pid < 0 || pid > INT32_MAX || endptr != pidbuf + STATIC_STRLEN(pidbuf) - 1) {
            continue;
        }

        errno = 0;
        if (kill(pid, 0) != 0 && errno == ESRCH) {
            if (unlink(lock_name) != 0) {
                continue;
            }

            // The process which owned this lock no longer exists. Try to acquire the lock again.
            --display;
            continue;
        }
    }

    ww_log(LOG_ERROR, "no X11 displays available");
    return -1;
}

static void
unlink_x11_display(int display) {
    char buf[64];

    snprintf(buf, STATIC_ARRLEN(buf), X11_SOCKET_FMT, display);
    unlink(buf);

    snprintf(buf, STATIC_ARRLEN(buf), X11_LOCK_FMT, display);
    unlink(buf);
}

static void
xserver_exec(struct xserver *srv, int notify_fd) {
    // This function should only ever be run in the context of the child process created from
    // `xserver_start`.

    // Unset CLOEXEC on the file descriptors which will be owned by the X server.
    const int fds[] = {
        srv->fd_x11[0],
        srv->fd_x11[1],
        srv->fd_xwm[1],
        srv->fd_wl[1],
    };

    for (size_t i = 0; i < STATIC_ARRLEN(fds); i++) {
        if (set_cloexec(fds[i], false) != 0) {
            return;
        }
    }

    // Build the command to pass to execvp.
    char *argv[64];
    size_t i = 0;

    char xfd0[16], xfd1[16], wmfd[16], displayfd[16];
    snprintf(xfd0, STATIC_ARRLEN(xfd0), "%d", srv->fd_x11[0]);
    snprintf(xfd1, STATIC_ARRLEN(xfd1), "%d", srv->fd_x11[1]);
    snprintf(wmfd, STATIC_ARRLEN(wmfd), "%d", srv->fd_xwm[1]);
    snprintf(displayfd, STATIC_ARRLEN(displayfd), "%d", notify_fd);

    argv[i++] = "Xwayland";
    argv[i++] = srv->display_name; // pre-acquired X11 display
    argv[i++] = "-rootless";       // run in rootless mode
    argv[i++] = "-core";           // make core dumps
    argv[i++] = "-noreset";        // do not reset when the last client disconnects

    argv[i++] = "-listenfd";
    argv[i++] = xfd0;
    argv[i++] = "-listenfd";
    argv[i++] = xfd1;

    argv[i++] = "-displayfd";
    argv[i++] = displayfd;

    argv[i++] = "-wm";
    argv[i++] = wmfd;

    argv[i++] = NULL;
    ww_assert(i < STATIC_ARRLEN(argv));

    char wayland_socket[16];
    snprintf(wayland_socket, STATIC_ARRLEN(wayland_socket), "%d", srv->fd_wl[1]);
    setenv("WAYLAND_SOCKET", wayland_socket, true);

    ww_log(LOG_INFO, "running Xwayland on display '%s'", srv->display_name);
    ww_assert(close(STDIN_FILENO) == 0);

    execvp(argv[0], argv);
    ww_log_errno(LOG_ERROR, "failed to exec Xwayland");
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
        goto fail;
    }

    // Create the readiness notification.
    struct wl_event_loop *loop = wl_display_get_event_loop(srv->wl_display);
    srv->src_pipe =
        wl_event_loop_add_fd(loop, notify_fd[0], WL_EVENT_READABLE, handle_xserver_ready, srv);

    // Spawn the child process.
    srv->pid = fork();
    if (srv->pid == 0) {
        // Child process
        xserver_exec(srv, notify_fd[1]);
        ww_log(LOG_ERROR, "failed to start xwayland");
        exit(EXIT_FAILURE);
    } else if (srv->pid == -1) {
        // Parent process (error)
        ww_log_errno(LOG_ERROR, "failed to fork xwayland");
        goto fail;
    }

    // The Xwayland process owns the other half of the displayfd pipe.
    // Close any file descriptors which the Xwayland process is supposed to own.
    close(srv->fd_x11[0]);
    close(srv->fd_x11[1]);
    close(srv->fd_wl[1]);
    close(srv->fd_xwm[1]);
    close(notify_fd[1]);

    srv->fd_x11[0] = srv->fd_x11[1] = -1;
    srv->fd_wl[1] = -1;
    srv->fd_xwm[1] = -1;

    srv->pidfd = pidfd_open(srv->pid, 0);
    if (srv->pidfd == -1) {
        ww_log_errno(LOG_ERROR, "failed to open pidfd");
    }

    srv->src_pidfd = wl_event_loop_add_fd(loop, srv->pidfd, WL_EVENT_READABLE, handle_pidfd, srv);
    check_alloc(srv->src_pidfd);

    return 0;

fail:
    close(notify_fd[0]);
    close(notify_fd[1]);
    return 1;
}

struct xserver *
xserver_create(struct server_xwayland *xwl) {
    struct xserver *srv = zalloc(1, sizeof(*srv));
    srv->wl_display = xwl->server->display;

    srv->fd_x11[0] = srv->fd_x11[1] = -1;
    srv->fd_xwm[0] = srv->fd_xwm[1] = -1;
    srv->fd_wl[0] = srv->fd_wl[1] = -1;

    srv->pidfd = -1;

    // Acquire an X11 display.
    srv->display = acquire_x11_display(srv->fd_x11);
    if (srv->display == -1) {
        goto fail;
    }
    ww_assert(snprintf(srv->display_name, STATIC_ARRLEN(srv->display_name), ":%d", srv->display) <=
              (ssize_t)STATIC_STRLEN(srv->display_name));

    // Create socket pairs for the Wayland connection and XWM connection.
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, srv->fd_wl) != 0) {
        ww_log_errno(LOG_ERROR, "failed to create wayland socket pair");
        goto fail;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, srv->fd_xwm) != 0) {
        ww_log_errno(LOG_ERROR, "failed to create xwm socket pair");
        goto fail;
    }

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

    safe_close(srv->fd_xwm[0]);
    safe_close(srv->fd_xwm[1]);
    safe_close(srv->fd_wl[0]);
    safe_close(srv->fd_wl[1]);
    safe_close(srv->fd_x11[0]);
    safe_close(srv->fd_x11[1]);

    if (srv->pidfd >= 0) {
        if (pidfd_send_signal(srv->pidfd, SIGKILL, NULL, 0) != 0) {
            ww_log_errno(LOG_ERROR, "failed to send SIGKILL to xserver");
        }
        close(srv->pidfd);
    }

    unlink_x11_display(srv->display);

    free(srv);
}
