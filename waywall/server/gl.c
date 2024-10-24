#include "glsl/main.frag.h"
#include "glsl/main.vert.h"

#include "linux-dmabuf-v1-client-protocol.h"
#include "server/backend.h"
#include "server/buffer.h"
#include "server/gl.h"
#include "server/server.h"
#include "server/ui.h"
#include "server/wl_compositor.h"
#include "server/wp_linux_dmabuf.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <spng.h>
#include <stdio.h>
#include <wayland-client-core.h>
#include <wayland-egl.h>

/*
 * This code is partially based off of weston, wlroots, and hello-wayland:
 *
 * ==== hello-wayland
 *
 *  Copyright (c) 2018 emersion
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
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

#define DRM_FORMAT_MOD_INVALID 0xFFFFFFFFFFFFFFFull

#define IMAGE_DRAWLIST_LEN 6
#define MAX_CACHED_DMABUF 2

#define ww_log_egl(lvl, fmt, ...)                                                                  \
    util_log(lvl, "[%s:%d] " fmt ": %s", __FILE__, __LINE__, ##__VA_ARGS__, egl_strerror())

#define VERTEX(options, dst_x, dst_y, src_x, src_y)                                                \
    (struct vertex) {                                                                              \
        .pos = {(dst_x), (dst_y)}, .tex = {(src_x), (src_y)},                                      \
        .src_rgba = {options->src_rgba[0], options->src_rgba[1], options->src_rgba[2],             \
                     options->src_rgba[3]},                                                        \
        .dst_rgba = {options->dst_rgba[0], options->dst_rgba[1], options->dst_rgba[2],             \
                     options->dst_rgba[3]},                                                        \
    }

struct gl_buffer {
    struct wl_list link; // server_gl.capture.buffers
    struct server_gl *gl;

    struct server_buffer *parent;
    EGLImageKHR image; // imported DMABUF - must not be modified
    GLuint texture;    // must not be modified
};

struct server_gl_image {
    struct wl_list link; // server_gl.draw.images
    struct server_gl *gl;

    int32_t width, height;

    char *drawlist;
    GLuint texture;
};

struct server_gl_mirror {
    struct wl_list link; // server_gl.draw.mirrors
    struct server_gl *gl;

    struct server_gl_mirror_options options;
};

struct vertex {
    float pos[2];
    float tex[2];
    float src_rgba[4];
    float dst_rgba[4];
};

// clang-format off
static const EGLint CONFIG_ATTRIBUTES[] = {
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_RED_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_BLUE_SIZE, 8,
    EGL_ALPHA_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_NONE,
};

static const EGLint CONTEXT_ATTRIBUTES[] = {
    EGL_CONTEXT_MAJOR_VERSION, 2,
    EGL_NONE,
};
// clang-format on

static const char *REQUIRED_EGL_EXTENSIONS[] = {
    "EGL_EXT_image_dma_buf_import",
    "EGL_EXT_image_dma_buf_import_modifiers",
    "EGL_KHR_image_base",
};

static const char *REQUIRED_GL_EXTENSIONS[] = {
    "GL_OES_EGL_image",
    "GL_OES_EGL_image_external",
};

static const struct {
    EGLint fd;
    EGLint offset;
    EGLint stride;
    EGLint mod_lo;
    EGLint mod_hi;
} DMABUF_ATTRIBUTES[] = {
    {
        EGL_DMA_BUF_PLANE0_FD_EXT,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
    },
    {
        EGL_DMA_BUF_PLANE1_FD_EXT,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT,
        EGL_DMA_BUF_PLANE1_PITCH_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
    },
    {
        EGL_DMA_BUF_PLANE2_FD_EXT,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT,
        EGL_DMA_BUF_PLANE2_PITCH_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
    },
    {
        EGL_DMA_BUF_PLANE3_FD_EXT,
        EGL_DMA_BUF_PLANE3_OFFSET_EXT,
        EGL_DMA_BUF_PLANE3_PITCH_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT,
    },
};

static const char *egl_strerror();
static bool gl_checkerr(const char *msg);

static void gl_buffer_destroy(struct gl_buffer *gl_buffer);
static struct gl_buffer *gl_buffer_import(struct server_gl *gl, struct server_buffer *buffer);

static void draw_build_lists(struct server_gl *gl);
static void draw_frame(struct server_gl *gl);

static const struct wl_callback_listener frame_cb_listener;

static void
on_frame_cb_done(void *data, struct wl_callback *callback, uint32_t time) {
    struct server_gl *gl = data;

    wl_callback_destroy(callback);

    gl->surface.frame_cb = wl_surface_frame(gl->surface.remote);
    check_alloc(gl->surface.frame_cb);
    wl_callback_add_listener(gl->surface.frame_cb, &frame_cb_listener, gl);

    draw_frame(gl);
}

static const struct wl_callback_listener frame_cb_listener = {
    .done = on_frame_cb_done,
};

static void
on_surface_commit(struct wl_listener *listener, void *data) {
    struct server_gl *gl = wl_container_of(listener, gl, on_surface_commit);

    struct server_buffer *buffer = server_surface_next_buffer(gl->capture.surface);
    if (!buffer) {
        gl->capture.current = NULL;
        return;
    }

    // Check if the committed wl_buffer has already been imported.
    struct gl_buffer *gl_buffer;
    wl_list_for_each (gl_buffer, &gl->capture.buffers, link) {
        if (gl_buffer->parent == buffer) {
            gl->capture.current = gl_buffer;
            return;
        }
    }

    // If the given wl_buffer has not yet been imported, try to import it.
    gl_buffer = gl_buffer_import(gl, buffer);
    if (!gl_buffer) {
        gl->capture.current = NULL;
        return;
    }

    // If there are too many cached buffers, remove the oldest one.
    if (wl_list_length(&gl->capture.buffers) > MAX_CACHED_DMABUF) {
        struct gl_buffer *buffer;
        wl_list_for_each_reverse (buffer, &gl->capture.buffers, link) {
            gl_buffer_destroy(buffer);
            break;
        }
    }

    gl->capture.current = gl_buffer;
}

static void
on_surface_destroy(struct wl_listener *listener, void *data) {
    struct server_gl *gl = wl_container_of(listener, gl, on_surface_destroy);

    gl->capture.current = NULL;
}

static void
on_ui_resize(struct wl_listener *listener, void *data) {
    struct server_gl *gl = wl_container_of(listener, gl, on_ui_resize);

    wl_egl_window_resize(gl->surface.window, gl->server->ui->width, gl->server->ui->height, 0, 0);
}

static bool
egl_getproc(void *out, const char *name) {
    void *addr = (void *)eglGetProcAddress(name);
    if (!addr) {
        ww_log(LOG_ERROR, "eglGetProcessAddress(\"%s\") failed", name);
        return false;
    }

    *((void **)out) = addr;
    return true;
}

static void
egl_print_sysinfo(struct server_gl *gl) {
    static const struct {
        EGLint name;
        const char *str;
    } EGL_STRINGS[] = {
        {EGL_CLIENT_APIS, "EGL_CLIENT_APIS"},
        {EGL_VENDOR, "EGL_VENDOR"},
        {EGL_VERSION, "EGL_VERSION"},
        {EGL_EXTENSIONS, "EGL_EXTENSIONS"},
    };

    static const struct {
        GLuint name;
        const char *str;
    } GL_STRINGS[] = {
        {GL_VENDOR, "GL_VENDOR"},
        {GL_RENDERER, "GL_RENDERER"},
        {GL_VERSION, "GL_VERSION"},
        {GL_SHADING_LANGUAGE_VERSION, "GL_SHADING_LANGUAGE_VERSION"},
        {GL_EXTENSIONS, "GL_EXTENSIONS"},
    };

    // Print EGL debug information.
    for (size_t i = 0; i < STATIC_ARRLEN(EGL_STRINGS); i++) {
        const char *value = eglQueryString(gl->egl.display, EGL_STRINGS[i].name);
        if (!value) {
            ww_log_egl(LOG_ERROR, "failed to query EGL string '%s'", EGL_STRINGS[i].str);
            continue;
        }

        ww_log(LOG_INFO, "%s = '%s'", EGL_STRINGS[i].str, value);
    }

    // Print OpenGL debug information.
    ww_assert(eglGetCurrentContext() == gl->egl.ctx);
    for (size_t i = 0; i < STATIC_ARRLEN(GL_STRINGS); i++) {
        const char *value = (const char *)glGetString(GL_STRINGS[i].name);
        if (!value) {
            ww_log(LOG_ERROR, "failed to query GL string '%s'", GL_STRINGS[i].str);
            continue;
        }

        ww_log(LOG_INFO, "%s = '%s'", GL_STRINGS[i].str, value);
    }
}

static const char *
egl_strerror() {
    static char buf[256];

    EGLint error = eglGetError();

    // clang-format off
    switch (error) {
    case EGL_SUCCESS:               return "EGL_SUCCESS";
    case EGL_NOT_INITIALIZED:       return "EGL_NOT_INITIALIZED";
    case EGL_BAD_ACCESS:            return "EGL_BAD_ACCESS";
    case EGL_BAD_ALLOC:             return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE:         return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONTEXT:           return "EGL_BAD_CONTEXT";
    case EGL_BAD_CONFIG:            return "EGL_BAD_CONFIG";
    case EGL_BAD_CURRENT_SURFACE:   return "EGL_BAD_CURRENT_SURFACE";
    case EGL_BAD_DISPLAY:           return "EGL_BAD_DISPLAY";
    case EGL_BAD_SURFACE:           return "EGL_BAD_SURFACE";
    case EGL_BAD_MATCH:             return "EGL_BAD_MATCH";
    case EGL_BAD_PARAMETER:         return "EGL_BAD_PARAMETER";
    case EGL_BAD_NATIVE_PIXMAP:     return "EGL_BAD_NATIVE_PIXMAP";
    case EGL_BAD_NATIVE_WINDOW:     return "EGL_BAD_NATIVE_WINDOW";
    case EGL_CONTEXT_LOST:          return "EGL_CONTEXT_LOST";
    default:
        snprintf(buf, STATIC_ARRLEN(buf), "unknown %jd", (intmax_t)error);
        return buf;
    }
    // clang-format on
}

static bool
gl_checkerr(const char *msg) {
    GLenum err = glGetError();
    if (err == GL_NO_ERROR) {
        return true;
    }

    ww_log(LOG_ERROR, "%s: %d", msg, (int)err);
    return false;
}

static void
gl_buffer_destroy(struct gl_buffer *gl_buffer) {
    server_buffer_unref(gl_buffer->parent);

    eglMakeCurrent(gl_buffer->gl->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   gl_buffer->gl->egl.ctx);

    glDeleteTextures(1, &gl_buffer->texture);
    gl_buffer->gl->egl.DestroyImageKHR(gl_buffer->gl->egl.display, gl_buffer->image);

    wl_list_remove(&gl_buffer->link);
    free(gl_buffer);
}

static struct gl_buffer *
gl_buffer_import(struct server_gl *gl, struct server_buffer *buffer) {
    if (strcmp(buffer->impl->name, SERVER_BUFFER_DMABUF) != 0) {
        ww_log(LOG_ERROR, "cannot create server_gl_surface for non-DMABUF buffer");
        return NULL;
    }

    struct gl_buffer *gl_buffer = zalloc(1, sizeof(*gl_buffer));
    gl_buffer->gl = gl;

    gl_buffer->parent = server_buffer_ref(buffer);

    // Attempt to create an EGLImageKHR for the given DMABUF.
    struct server_dmabuf_data *data = buffer->data;

    bool has_modifier = (data->modifier_lo != (DRM_FORMAT_MOD_INVALID & 0xFFFFFFFF) ||
                         data->modifier_hi != ((DRM_FORMAT_MOD_INVALID >> 32) & 0xFFFFFFFF));

    EGLint image_attributes[64];
    int i = 0;

    image_attributes[i++] = EGL_WIDTH;
    image_attributes[i++] = data->width;
    image_attributes[i++] = EGL_HEIGHT;
    image_attributes[i++] = data->height;
    image_attributes[i++] = EGL_LINUX_DRM_FOURCC_EXT;
    image_attributes[i++] = data->format;

    for (uint32_t p = 0; p < data->num_planes; p++) {
        image_attributes[i++] = DMABUF_ATTRIBUTES[p].fd;
        image_attributes[i++] = data->planes[p].fd;
        image_attributes[i++] = DMABUF_ATTRIBUTES[p].offset;
        image_attributes[i++] = data->planes[p].offset;
        image_attributes[i++] = DMABUF_ATTRIBUTES[p].stride;
        image_attributes[i++] = data->planes[p].stride;

        // TODO: There are more checks to do w.r.t. DRM formats. See wlroots' render/egl.c for
        // details.
        if (has_modifier) {
            image_attributes[i++] = DMABUF_ATTRIBUTES[p].mod_lo;
            image_attributes[i++] = data->modifier_lo;
            image_attributes[i++] = DMABUF_ATTRIBUTES[p].mod_hi;
            image_attributes[i++] = data->modifier_hi;
        }
    }

    image_attributes[i++] = EGL_IMAGE_PRESERVED_KHR;
    image_attributes[i++] = EGL_TRUE;

    image_attributes[i++] = EGL_NONE;

    gl_buffer->image = gl_buffer->gl->egl.CreateImageKHR(
        gl_buffer->gl->egl.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, image_attributes);
    if (gl_buffer->image == EGL_NO_IMAGE_KHR) {
        ww_log_egl(LOG_ERROR, "failed to create EGLImage for dmabuf");
        goto fail_create_orig;
    }

    // Create an OpenGL texture with the imported EGLImageKHR.
    if (!eglMakeCurrent(gl_buffer->gl->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        gl_buffer->gl->egl.ctx)) {
        ww_log_egl(LOG_ERROR, "failed to make EGL context current");
        goto fail_make_current;
    }

    glGenTextures(1, &gl_buffer->texture);
    glBindTexture(GL_TEXTURE_2D, gl_buffer->texture);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    gl_buffer->gl->egl.ImageTargetTexture2DOES(GL_TEXTURE_2D, gl_buffer->image);
    if (!gl_checkerr("failed to import texture")) {
        goto fail_image_target;
    }

    wl_list_insert(&gl->capture.buffers, &gl_buffer->link);

    return gl_buffer;

fail_image_target:
fail_make_current:
    gl_buffer->gl->egl.DestroyImageKHR(gl_buffer->gl->egl.display, gl_buffer->image);

fail_create_orig:
    server_buffer_unref(gl_buffer->parent);
    free(gl_buffer);

    return NULL;
}

static struct vertex *
draw_build_rect(struct vertex *buf, struct server_gl_mirror_options *o) {
    const struct box *s = &o->src;
    const struct box *d = &o->dst;

    // Top-left triangle
    *(buf++) = VERTEX(o, d->x, d->y, s->x, s->y);
    *(buf++) = VERTEX(o, d->x + d->width, d->y, s->x + s->width, s->y);
    *(buf++) = VERTEX(o, d->x, d->y + d->height, s->x, s->y + s->height);

    // Bottom-right triangle
    *(buf++) = VERTEX(o, d->x + d->width, d->y, s->x + s->width, s->y);
    *(buf++) = VERTEX(o, d->x, d->y + d->height, s->x, s->y + s->height);
    *(buf++) = VERTEX(o, d->x + d->width, d->y + d->height, s->x + s->width, s->y + s->height);

    return buf;
}

static void
draw_build_lists(struct server_gl *gl) {
    if (wl_list_empty(&gl->draw.mirrors)) {
        return;
    }

    // Allocate space for the vertices.
    size_t cap = sizeof(struct vertex) * 6 * wl_list_length(&gl->draw.mirrors);
    gl->draw.buf = realloc(gl->draw.buf, cap);
    check_alloc(gl->draw.buf);
    gl->draw.len = cap;

    // Fill the vertex buffer.
    struct vertex *ptr = (struct vertex *)gl->draw.buf;

    struct server_gl_mirror *mirror;
    wl_list_for_each (mirror, &gl->draw.mirrors, link) {
        ptr = draw_build_rect(ptr, &mirror->options);
    }
}

static void
draw_list(struct server_gl *gl, struct vertex *vertices, size_t num_vertices, GLuint texture) {
    glBindBuffer(GL_ARRAY_BUFFER, gl->shader.vbo);
    glBufferData(GL_ARRAY_BUFFER, num_vertices * sizeof(struct vertex), vertices, GL_STREAM_DRAW);

    glVertexAttribPointer(gl->shader.attrib_pos, 2, GL_FLOAT, GL_FALSE, sizeof(struct vertex),
                          (const void *)offsetof(struct vertex, pos));
    glVertexAttribPointer(gl->shader.attrib_tex, 2, GL_FLOAT, GL_FALSE, sizeof(struct vertex),
                          (const void *)offsetof(struct vertex, tex));
    glVertexAttribPointer(gl->shader.attrib_src_rgba, 4, GL_FLOAT, GL_FALSE, sizeof(struct vertex),
                          (const void *)offsetof(struct vertex, src_rgba));
    glVertexAttribPointer(gl->shader.attrib_dst_rgba, 4, GL_FLOAT, GL_FALSE, sizeof(struct vertex),
                          (const void *)offsetof(struct vertex, dst_rgba));

    glEnableVertexAttribArray(gl->shader.attrib_pos);
    glEnableVertexAttribArray(gl->shader.attrib_tex);
    glEnableVertexAttribArray(gl->shader.attrib_src_rgba);
    glEnableVertexAttribArray(gl->shader.attrib_dst_rgba);

    glBindTexture(GL_TEXTURE_2D, texture);
    glDrawArrays(GL_TRIANGLES, 0, num_vertices);

    glDisableVertexAttribArray(gl->shader.attrib_pos);
    glDisableVertexAttribArray(gl->shader.attrib_tex);
    glDisableVertexAttribArray(gl->shader.attrib_src_rgba);
    glDisableVertexAttribArray(gl->shader.attrib_dst_rgba);
}

static void
draw_frame(struct server_gl *gl) {
    eglMakeCurrent(gl->egl.display, gl->surface.egl, gl->surface.egl, gl->egl.ctx);

    // If there is nothing to draw, then clear the framebuffer and return.
    if (!gl->capture.current || gl->draw.len == 0) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glClearColor(0, 0, 0, 0);

        eglSwapBuffers(gl->egl.display, gl->surface.egl);
        return;
    }

    // Otherwise, draw as normal.
    int32_t ui_width = gl->server->ui->width;
    int32_t ui_height = gl->server->ui->height;
    glViewport(0, 0, ui_width, ui_height);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glClearColor(0, 0, 0, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(gl->shader.prog);

    int32_t buf_width, buf_height;
    server_buffer_get_size(gl->capture.current->parent, &buf_width, &buf_height);
    glUniform2f(gl->shader.uniform_winsize, ui_width, ui_height);

    // Draw all mirrors.
    glUniform2f(gl->shader.uniform_texsize, buf_width, buf_height);
    draw_list(gl, (struct vertex *)gl->draw.buf, gl->draw.len / sizeof(struct vertex),
              gl->capture.current->texture);

    // Draw all images.
    struct server_gl_image *image;
    wl_list_for_each (image, &gl->draw.images, link) {
        glUniform2f(gl->shader.uniform_texsize, image->width, image->height);
        draw_list(gl, (struct vertex *)image->drawlist, IMAGE_DRAWLIST_LEN, image->texture);
    }

    // Push the results to the framebuffer.
    eglSwapBuffers(gl->egl.display, gl->surface.egl);

    // TODO: Explicit sync support
}

static bool
gl_link_program(struct server_gl *gl) {
    gl->shader.prog = glCreateProgram();
    ww_assert(gl->shader.prog != 0);

    glAttachShader(gl->shader.prog, gl->shader.vert);
    glAttachShader(gl->shader.prog, gl->shader.frag);
    glLinkProgram(gl->shader.prog);

    GLint status;
    glGetProgramiv(gl->shader.prog, GL_LINK_STATUS, &status);
    if (status == GL_TRUE) {
        return true;
    }

    char buf[4096] = {0};
    glGetProgramInfoLog(gl->shader.prog, STATIC_ARRLEN(buf) - 1, NULL, buf);
    ww_log(LOG_ERROR, "failed to link shader program: %s", buf);

    return false;
}

static bool
gl_make_shader(GLuint *output, GLint type, const char *src) {
    *output = glCreateShader(type);
    ww_assert(*output != 0);

    glShaderSource(*output, 1, &src, NULL);
    glCompileShader(*output);

    GLint status;
    glGetShaderiv(*output, GL_COMPILE_STATUS, &status);
    if (status == GL_TRUE) {
        return true;
    }

    char buf[4096] = {0};
    glGetShaderInfoLog(*output, STATIC_ARRLEN(buf) - 1, NULL, buf);
    ww_log(LOG_ERROR, "failed to compile shader: %s", buf);

    return false;
}

struct server_gl *
server_gl_create(struct server *server) {
    struct server_gl *gl = zalloc(1, sizeof(*gl));

    gl->server = server;

    // Initialize the EGL display.
    if (!egl_getproc(&gl->egl.GetPlatformDisplayEXT, "eglGetPlatformDisplayEXT")) {
        goto fail_get_proc;
    }
    if (!egl_getproc(&gl->egl.CreatePlatformWindowSurfaceEXT,
                     "eglCreatePlatformWindowSurfaceEXT")) {
        goto fail_get_proc;
    }

    gl->egl.display =
        gl->egl.GetPlatformDisplayEXT(EGL_PLATFORM_WAYLAND_EXT, gl->server->backend->display, NULL);
    if (gl->egl.display == EGL_NO_DISPLAY) {
        ww_log(LOG_ERROR, "no EGL display available");
        goto fail_get_display;
    }

    if (!eglInitialize(gl->egl.display, &gl->egl.major, &gl->egl.minor)) {
        ww_log_egl(LOG_ERROR, "failed to initialize EGL");
        goto fail_initialize;
    }

    // Query for EGL extension support. In particular, we need to be able to import and export
    // DMABUFs.
    const char *egl_extensions = eglQueryString(gl->egl.display, EGL_EXTENSIONS);
    if (!egl_extensions) {
        ww_log_egl(LOG_ERROR, "failed to query EGL extensions");
        goto fail_extensions_egl;
    }

    for (size_t i = 0; i < STATIC_ARRLEN(REQUIRED_EGL_EXTENSIONS); i++) {
        if (!strstr(egl_extensions, REQUIRED_EGL_EXTENSIONS[i])) {
            ww_log(LOG_ERROR, "no support for '%s'", REQUIRED_EGL_EXTENSIONS[i]);
            goto fail_extensions_egl;
        }
    }

    if (!egl_getproc(&gl->egl.CreateImageKHR, "eglCreateImageKHR")) {
        goto fail_extensions_egl;
    }
    if (!egl_getproc(&gl->egl.DestroyImageKHR, "eglDestroyImageKHR")) {
        goto fail_extensions_egl;
    }
    if (!egl_getproc(&gl->egl.ImageTargetTexture2DOES, "glEGLImageTargetTexture2DOES")) {
        goto fail_extensions_egl;
    }

    // Choose a configuration and create an EGL context.
    EGLint n = 0;
    if (!eglChooseConfig(gl->egl.display, CONFIG_ATTRIBUTES, &gl->egl.config, 1, &n) || n == 0) {
        ww_log_egl(LOG_ERROR, "failed to choose EGL config");
        goto fail_choose_config;
    }

    gl->egl.ctx =
        eglCreateContext(gl->egl.display, gl->egl.config, EGL_NO_CONTEXT, CONTEXT_ATTRIBUTES);
    if (gl->egl.ctx == EGL_NO_CONTEXT) {
        ww_log(LOG_ERROR, "failed to create EGL context");
        goto fail_create_context;
    }

    // Ensure the required OpenGL extensions are present.
    if (!eglMakeCurrent(gl->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, gl->egl.ctx)) {
        ww_log_egl(LOG_ERROR, "failed to make EGL context current");
        goto fail_make_current;
    }

    const char *gl_extensions = (const char *)glGetString(GL_EXTENSIONS);
    if (!gl_extensions) {
        ww_log_egl(LOG_ERROR, "failed to query GL extensions");
        goto fail_extensions_gl;
    }

    for (size_t i = 0; i < STATIC_ARRLEN(REQUIRED_GL_EXTENSIONS); i++) {
        if (!strstr(gl_extensions, REQUIRED_GL_EXTENSIONS[i])) {
            ww_log(LOG_ERROR, "no support for '%s'", REQUIRED_EGL_EXTENSIONS[i]);
            goto fail_extensions_gl;
        }
    }

    // Create the shader program and allocate buffers.
    if (!gl_make_shader(&gl->shader.vert, GL_VERTEX_SHADER, WAYWALL_GLSL_MAIN_VERT_H)) {
        goto fail_vert_shader;
    }
    if (!gl_make_shader(&gl->shader.frag, GL_FRAGMENT_SHADER, WAYWALL_GLSL_MAIN_FRAG_H)) {
        goto fail_frag_shader;
    }
    if (!gl_link_program(gl)) {
        goto fail_link_program;
    }

    gl->shader.attrib_pos = glGetAttribLocation(gl->shader.prog, "v_pos");
    ww_assert(gl->shader.attrib_pos != -1);

    gl->shader.attrib_tex = glGetAttribLocation(gl->shader.prog, "v_tex");
    ww_assert(gl->shader.attrib_tex != -1);

    gl->shader.attrib_src_rgba = glGetAttribLocation(gl->shader.prog, "v_src_rgba");
    ww_assert(gl->shader.attrib_src_rgba != -1);

    gl->shader.attrib_dst_rgba = glGetAttribLocation(gl->shader.prog, "v_dst_rgba");
    ww_assert(gl->shader.attrib_dst_rgba != -1);

    gl->shader.uniform_texsize = glGetUniformLocation(gl->shader.prog, "u_texsize");
    ww_assert(gl->shader.uniform_texsize != -1);

    gl->shader.uniform_winsize = glGetUniformLocation(gl->shader.prog, "u_winsize");
    ww_assert(gl->shader.uniform_winsize != -1);

    glGenBuffers(1, &gl->shader.vbo);

    // Create the OpenGL surface.
    gl->surface.remote = wl_compositor_create_surface(server->backend->compositor);
    check_alloc(gl->surface.remote);
    wl_surface_set_input_region(gl->surface.remote, server->ui->empty_region);

    gl->surface.subsurface = wl_subcompositor_get_subsurface(
        server->backend->subcompositor, gl->surface.remote, server->ui->tree.surface);
    check_alloc(gl->surface.subsurface);
    wl_subsurface_set_desync(gl->surface.subsurface);

    gl->surface.frame_cb = wl_surface_frame(gl->surface.remote);
    check_alloc(gl->surface.frame_cb);
    wl_callback_add_listener(gl->surface.frame_cb, &frame_cb_listener, gl);

    // Use arbitrary sizes here since the main UI window has not yet been sized.
    gl->surface.window = wl_egl_window_create(gl->surface.remote, 1, 1);
    check_alloc(gl->surface.window);

    gl->surface.egl = gl->egl.CreatePlatformWindowSurfaceEXT(gl->egl.display, gl->egl.config,
                                                             gl->surface.window, NULL);
    if (gl->surface.egl == EGL_NO_SURFACE) {
        ww_log_egl(LOG_ERROR, "failed to create EGL surface");
        goto fail_egl_surface;
    }

    eglMakeCurrent(gl->egl.display, gl->surface.egl, gl->surface.egl, gl->egl.ctx);
    eglSwapInterval(gl->egl.display, 0);

    wl_surface_commit(gl->surface.remote);
    draw_frame(gl);

    gl->on_ui_resize.notify = on_ui_resize;
    wl_signal_add(&server->ui->events.resize, &gl->on_ui_resize);

    // Log debug information about the user's EGL/OpenGL implementation.
    egl_print_sysinfo(gl);

    wl_list_init(&gl->draw.mirrors);
    wl_list_init(&gl->draw.images);
    wl_list_init(&gl->capture.buffers);

    return gl;

fail_egl_surface:
    wl_egl_window_destroy(gl->surface.window);
    wl_callback_destroy(gl->surface.frame_cb);
    wl_subsurface_destroy(gl->surface.subsurface);
    wl_surface_destroy(gl->surface.remote);

    glDeleteBuffers(1, &gl->shader.vbo);
    glDeleteProgram(gl->shader.prog);

fail_link_program:
    glDeleteShader(gl->shader.frag);

fail_frag_shader:
    glDeleteShader(gl->shader.vert);

fail_vert_shader:
fail_extensions_gl:
fail_make_current:
    eglMakeCurrent(gl->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(gl->egl.display, gl->egl.ctx);

fail_create_context:
fail_choose_config:
fail_extensions_egl:
    eglTerminate(gl->egl.display);

fail_initialize:
fail_get_display:
fail_get_proc:
    free(gl);

    return NULL;
}

void
server_gl_destroy(struct server_gl *gl) {
    eglMakeCurrent(gl->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, gl->egl.ctx);

    // Destroy capture resources.
    if (gl->capture.surface) {
        wl_list_remove(&gl->on_surface_commit.link);
        wl_list_remove(&gl->on_surface_destroy.link);
    }

    struct gl_buffer *gl_buffer, *gl_buffer_tmp;
    wl_list_for_each_safe (gl_buffer, gl_buffer_tmp, &gl->capture.buffers, link) {
        gl_buffer_destroy(gl_buffer);
    }

    struct server_gl_mirror *mirror, *mirror_tmp;
    wl_list_for_each_safe (mirror, mirror_tmp, &gl->draw.mirrors, link) {
        mirror->gl = NULL;

        wl_list_remove(&mirror->link);
        wl_list_init(&mirror->link);
    }

    struct server_gl_image *image, *image_tmp;
    wl_list_for_each_safe (image, image_tmp, &gl->draw.images, link) {
        image->gl = NULL;

        eglMakeCurrent(gl->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, gl->egl.ctx);
        glDeleteTextures(1, &image->texture);

        wl_list_remove(&image->link);
        wl_list_init(&image->link);
    }

    // Destroy surface resources.
    wl_list_remove(&gl->on_ui_resize.link);

    eglDestroySurface(gl->egl.display, gl->surface.egl);
    wl_egl_window_destroy(gl->surface.window);
    wl_subsurface_destroy(gl->surface.subsurface);
    wl_surface_destroy(gl->surface.remote);

    if (gl->surface.frame_cb) {
        wl_callback_destroy(gl->surface.frame_cb);
    }

    // Destroy OpenGL resources.
    glDeleteBuffers(1, &gl->shader.vbo);
    glDeleteProgram(gl->shader.prog);
    glDeleteShader(gl->shader.frag);
    glDeleteShader(gl->shader.vert);

    // Destroy EGL resources.
    eglMakeCurrent(gl->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(gl->egl.display, gl->egl.ctx);
    eglTerminate(gl->egl.display);

    if (gl->draw.buf) {
        free(gl->draw.buf);
    }
    free(gl);
}

void
server_gl_set_target(struct server_gl *gl, struct server_surface *surface) {
    if (gl->capture.surface) {
        ww_panic("cannot overwrite capture surface");
    }

    gl->capture.surface = surface;

    gl->on_surface_commit.notify = on_surface_commit;
    wl_signal_add(&surface->events.commit, &gl->on_surface_commit);

    gl->on_surface_destroy.notify = on_surface_destroy;
    wl_signal_add(&surface->events.destroy, &gl->on_surface_destroy);
}

struct server_gl_mirror *
server_gl_mirror_create(struct server_gl *gl, struct server_gl_mirror_options options) {
    struct server_gl_mirror *mirror = zalloc(1, sizeof(*mirror));

    mirror->gl = gl;
    mirror->options = options;

    wl_list_insert(&gl->draw.mirrors, &mirror->link);
    draw_build_lists(gl);

    return mirror;
}

void
server_gl_mirror_destroy(struct server_gl_mirror *mirror) {
    wl_list_remove(&mirror->link);

    if (mirror->gl) {
        draw_build_lists(mirror->gl);
    }

    free(mirror);
}

struct server_gl_image *
server_gl_image_create(struct server_gl *gl, char *buf, size_t bufsize,
                       struct server_gl_mirror_options options) {
    struct server_gl_image *image = zalloc(1, sizeof(*image));

    image->gl = gl;

    // Attempt to decode the PNG.
    spng_ctx *ctx = spng_ctx_new(0);
    if (!ctx) {
        ww_log(LOG_ERROR, "failed to create spng_ctx");
        goto fail_ctx_new;
    }

    int err = spng_set_png_buffer(ctx, buf, bufsize);
    if (err != 0) {
        ww_log(LOG_ERROR, "failed to set png buffer: %s\n", spng_strerror(err));
        goto fail_set_png_buffer;
    }

    struct spng_ihdr ihdr = {0};
    err = spng_get_ihdr(ctx, &ihdr);
    if (err != 0) {
        ww_log(LOG_ERROR, "failed to get image header: %s\n", spng_strerror(err));
        goto fail_get_ihdr;
    }

    size_t decode_size;
    err = spng_decoded_image_size(ctx, SPNG_FMT_RGBA8, &decode_size);
    if (err != 0) {
        ww_log(LOG_ERROR, "failed to get decoded image size: %s\n", spng_strerror(err));
        goto fail_get_image_size;
    }

    char *decode_buf = zalloc(decode_size, 1);
    err = spng_decode_image(ctx, decode_buf, decode_size, SPNG_FMT_RGBA8, SPNG_DECODE_TRNS);
    if (err != 0) {
        ww_log(LOG_ERROR, "failed to decode image: %s\n", spng_strerror(err));
        goto fail_decode_image;
    }

    // Create a buffer containing the vertex data for the image.
    options.src = (struct box){0, 0, ihdr.width, ihdr.height};

    image->width = ihdr.width;
    image->height = ihdr.height;

    image->drawlist = zalloc(IMAGE_DRAWLIST_LEN, sizeof(struct vertex));
    draw_build_rect((struct vertex *)image->drawlist, &options);

    // Attempt to place the PNG into an OpenGL texture.
    eglMakeCurrent(gl->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, gl->egl.ctx);

    glGenTextures(1, &image->texture);
    glBindTexture(GL_TEXTURE_2D, image->texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ihdr.width, ihdr.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 decode_buf);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Cleanup the PNG decoder resources.
    free(decode_buf);
    spng_ctx_free(ctx);

    wl_list_insert(&gl->draw.images, &image->link);

    return image;

fail_decode_image:
    free(decode_buf);

fail_get_image_size:
fail_get_ihdr:
fail_set_png_buffer:
    spng_ctx_free(ctx);

fail_ctx_new:
    free(image);

    return NULL;
}

void
server_gl_image_destroy(struct server_gl_image *image) {
    wl_list_remove(&image->link);

    if (image->gl) {
        eglMakeCurrent(image->gl->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, image->gl->egl.ctx);
        glDeleteTextures(1, &image->texture);
    }

    free(image->drawlist);
    free(image);
}
