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
    struct server *server;

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

    struct {
        GLuint frag, vert, prog;

        GLuint vbo;
        GLint attrib_pos, attrib_tex, attrib_src_rgba, attrib_dst_rgba;
        GLint uniform_texsize, uniform_winsize;
    } shader;

    struct {
        struct wl_surface *remote;
        struct wl_subsurface *subsurface;
        struct wl_egl_window *window;
        EGLSurface egl;

        struct wl_callback *frame_cb;
    } surface;

    struct {
        struct server_surface *surface;
        struct wl_list buffers; // gl_buffer.link
        struct gl_buffer *current;
    } capture;

    struct {
        char *buf;
        size_t len;

        struct wl_list mirrors; // server_gl_mirror.link
    } draw;

    struct wl_listener on_surface_commit;
    struct wl_listener on_surface_destroy;
    struct wl_listener on_ui_resize;
};

struct server_gl_mirror;

struct server_gl_mirror_options {
    struct box src, dst;
    float src_rgba[4], dst_rgba[4];
};

struct server_gl *server_gl_create(struct server *server);
void server_gl_destroy(struct server_gl *gl);
void server_gl_set_target(struct server_gl *gl, struct server_surface *surface);

struct server_gl_mirror *server_gl_mirror_create(struct server_gl *gl,
                                                 struct server_gl_mirror_options options);
void server_gl_mirror_destroy(struct server_gl_mirror *mirror);

#endif
