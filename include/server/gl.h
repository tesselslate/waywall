#ifndef WAYWALL_SERVER_GL_H
#define WAYWALL_SERVER_GL_H

#include <EGL/egl.h>
#include <EGL/eglext.h>

struct server_gl {
    struct {
        PFNEGLCREATEIMAGEKHRPROC CreateImageKHR;
        PFNEGLDESTROYIMAGEKHRPROC DestroyImageKHR;
        PFNEGLGETPLATFORMDISPLAYEXTPROC GetPlatformDisplayEXT;
        PFNEGLEXPORTDMABUFIMAGEMESAPROC ExportDMABUFImageMESA;
        PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC ExportDMABUFImageQueryMESA;

        EGLDisplay display;
        EGLConfig config;
        EGLContext ctx;
        EGLint major, minor;
    } egl;

    struct server *server;
};

struct server_gl *server_gl_create(struct server *server);
void server_gl_destroy(struct server_gl *gl);

#endif
