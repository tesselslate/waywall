#ifndef WAYWALL_SERVER_GL_H
#define WAYWALL_SERVER_GL_H

#include "util/box.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdbool.h>
#include <wayland-server-core.h>

struct server_gl {
    struct {
        PFNEGLCREATEIMAGEKHRPROC CreateImageKHR;
        PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC CreatePlatformWindowSurfaceEXT;
        PFNEGLDESTROYIMAGEKHRPROC DestroyImageKHR;
        PFNEGLGETPLATFORMDISPLAYEXTPROC GetPlatformDisplayEXT;
        PFNGLEGLIMAGETARGETTEXTURE2DOESPROC ImageTargetTexture2DOES;

        EGLDisplay display;
        EGLConfig config;
        EGLContext ctx;
        EGLint major, minor;
    } egl;

    struct server *server;

    struct {
        GLuint frag, vert, prog;

        GLuint vbo;
        GLint attrib_pos, attrib_texcoord;
        GLint uniform_size, uniform_key_src, uniform_key_dst;
    } shader;
};

struct server_gl_surface {
    struct server_gl *gl;
    struct server_surface *parent;

    struct wl_surface *remote;
    struct wl_egl_window *window;
    EGLSurface egl_surface;

    struct wl_list buffers; // gl_buffer.link
    struct gl_buffer *current;

    struct server_gl_surface_options {
        struct box crop;
        int32_t width, height;
        float src_rgba[4], dst_rgba[4];
    } options;

    struct wl_listener on_surface_commit;
    struct wl_listener on_surface_destroy;
};

struct server_gl *server_gl_create(struct server *server);
void server_gl_destroy(struct server_gl *gl);

struct server_gl_surface *server_gl_surface_create(struct server_gl *gl,
                                                   struct server_surface *surface,
                                                   struct server_gl_surface_options options);
void server_gl_surface_destroy(struct server_gl_surface *gl_surface);

#endif
