#ifndef WAYWALL_GL_H
#define WAYWALL_GL_H

#include <EGL/egl.h>
#include <EGL/eglext.h>

struct ww_gl {
    struct {
        PFNEGLGETPLATFORMDISPLAYEXTPROC GetPlatformDisplayEXT;

        EGLDisplay display;
        EGLConfig config;
        EGLContext ctx;
        EGLint major, minor;
    } egl;

    struct wl_display *remote_display;
};

struct server;

struct ww_gl *ww_gl_create(struct server *server);
void ww_gl_destroy(struct ww_gl *gl);

#endif
