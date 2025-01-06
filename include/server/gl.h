#ifndef WAYWALL_SERVER_GL_H
#define WAYWALL_SERVER_GL_H

#include "util/box.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdbool.h>
#include <wayland-server-core.h>

#define server_gl_with(gl, surface)                                                                \
    for (int _glscope = (server_gl_enter((gl), (surface)), 0); _glscope == 0;                      \
         _glscope = (server_gl_exit((gl)), 1))

#define gl_using_buffer(type, buffer)                                                              \
    for (int _gl_bufscope = (glBindBuffer((type), (buffer)), 0); _gl_bufscope == 0;                \
         _gl_bufscope = (glBindBuffer((type), 0), 1))

#define gl_using_texture(type, texture)                                                            \
    for (int _gl_texscope = (glBindTexture((type), (texture)), 0); _gl_texscope == 0;              \
         _gl_texscope = (glBindTexture((type), 0), 1))

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
        struct wl_surface *remote;
        struct wl_subsurface *subsurface;
        struct wl_egl_window *window;
        EGLSurface egl;
    } surface;

    struct {
        struct server_surface *surface;
        struct wl_list buffers; // gl_buffer.link
        struct gl_buffer *current;
    } capture;

    struct wl_listener on_surface_commit;
    struct wl_listener on_surface_destroy;
    struct wl_listener on_ui_resize;

    struct {
        struct wl_signal frame; // data: NULL
    } events;
};

struct server_gl_shader {
    GLuint vert, frag;
    GLuint program;
};

struct server_gl *server_gl_create(struct server *server);
void server_gl_destroy(struct server_gl *gl);
void server_gl_enter(struct server_gl *gl, bool surface);
void server_gl_exit(struct server_gl *gl);

struct server_gl_shader *server_gl_compile(struct server_gl *gl, const char *vertex,
                                           const char *fragment);
GLuint server_gl_get_capture(struct server_gl *gl);
void server_gl_get_capture_size(struct server_gl *gl, int32_t *width, int32_t *height);
void server_gl_set_capture(struct server_gl *gl, struct server_surface *surface);
void server_gl_swap_buffers(struct server_gl *gl);

void server_gl_shader_destroy(struct server_gl_shader *shader);
void server_gl_shader_use(struct server_gl_shader *shader);

#endif
