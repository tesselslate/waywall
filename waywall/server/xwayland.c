#include "server/xwayland.h"
#include "server/server.h"
#include "server/xserver.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"

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

static void
on_ready(struct wl_listener *listener, void *data) {
    struct server_xwayland *xwl = wl_container_of(listener, xwl, on_ready);

#warning TODO seat
#warning TODO cursor

    wl_signal_emit_mutable(&xwl->events.ready, NULL);
}

static void
on_display_destroy(struct wl_listener *listener, void *data) {
    struct server_xwayland *xwl = wl_container_of(listener, xwl, on_display_destroy);

    xserver_destroy(xwl->xserver);

    wl_list_remove(&xwl->on_display_destroy.link);

    free(xwl);
}

struct server_xwayland *
server_xwayland_create(struct server *server) {
    struct server_xwayland *xwl = zalloc(1, sizeof(*xwl));

    xwl->server = server;

    xwl->xserver = xserver_create(xwl);
    if (!xwl->xserver) {
        free(xwl);
        return NULL;
    }

    wl_signal_init(&xwl->events.ready);

    xwl->on_ready.notify = on_ready;
    wl_signal_add(&xwl->xserver->events.ready, &xwl->on_ready);

    xwl->on_display_destroy.notify = on_display_destroy;
    wl_display_add_destroy_listener(server->display, &xwl->on_display_destroy);

    return xwl;
}
