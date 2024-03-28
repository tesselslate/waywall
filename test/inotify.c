#include "inotify.h"
#include "util.h"
#include <fcntl.h>
#include <stdio.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <wayland-server-core.h>

struct shared {
    struct wl_display *display;
    bool ok;
};

static int
make_file(char *namebuf) {
    sprintf(namebuf, "/tmp/inotify-test-%d", getpid());
    return open(namebuf, O_CREAT | O_WRONLY, 0644);
}

static void
listen(int wd, uint32_t mask, const char *name, void *rawdata) {
    struct shared *data = rawdata;
    data->ok = true;
    wl_display_terminate(data->display);
}

int
main() {
    struct wl_display *display = wl_display_create();
    struct wl_event_loop *loop = wl_display_get_event_loop(display);

    struct inotify *inotify = inotify_create(loop);
    ww_assert(inotify);

    char buf[128];
    int fd = make_file(buf);
    ww_assert(fd >= 0);

    struct shared data = {.display = display, .ok = false};
    int wd = inotify_subscribe(inotify, buf, IN_MODIFY, listen, &data);
    ww_assert(wd >= 0);

    ww_assert(write(fd, buf, strlen(buf)) == (ssize_t)strlen(buf));
    wl_display_run(display);

    inotify_unsubscribe(inotify, wd);

    inotify_destroy(inotify);
    wl_display_destroy(display);

    ww_assert(data.ok);
    close(fd);
}
