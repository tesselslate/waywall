#include "glsl/main.frag.h"
#include "glsl/main.vert.h"

#include "linux-dmabuf-v1-client-protocol.h"
#include "server/backend.h"
#include "server/buffer.h"
#include "server/gl.h"
#include "server/server.h"
#include "server/wl_compositor.h"
#include "server/wp_linux_dmabuf.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
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

#define ww_log_egl(lvl, fmt, ...)                                                                  \
    util_log(lvl, "[%s:%d] " fmt ": %s", __FILE__, __LINE__, ##__VA_ARGS__, egl_strerror())

struct gl_buffer {
    struct wl_list link; // server_gl_surface.buffers
    struct server_gl *gl;

    struct server_buffer *parent;
    EGLImageKHR image; // imported DMABUF - must not be modified
    GLuint texture;    // must not be modified
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
static struct gl_buffer *gl_buffer_import(struct server_gl_surface *gl_surface,
                                          struct server_buffer *buffer);
static void gl_surface_update(struct server_gl_surface *gl_surface);

static void
on_gl_surface_commit(struct wl_listener *listener, void *data) {
    struct server_gl_surface *gl_surface = wl_container_of(listener, gl_surface, on_surface_commit);

    struct server_buffer *buffer = server_surface_next_buffer(gl_surface->parent);
    if (!buffer) {
        gl_surface->current = NULL;
        return;
    }

    // Check if the committed wl_buffer has already been imported by this surface before.
    struct gl_buffer *gl_buffer;
    bool has_gl_buffer = false;
    wl_list_for_each (gl_buffer, &gl_surface->buffers, link) {
        if (gl_buffer->parent == buffer) {
            has_gl_buffer = true;
            break;
        }
    }

    // If the given wl_buffer needs to be imported, try to import it.
    if (!has_gl_buffer) {
        gl_buffer = gl_buffer_import(gl_surface, buffer);
        if (!gl_buffer) {
            gl_surface->current = NULL;
            return;
        }
    }

    gl_surface->current = gl_buffer;

    gl_surface_update(gl_surface);
}

static void
on_gl_surface_destroy(struct wl_listener *listener, void *data) {
    struct server_gl_surface *gl_surface =
        wl_container_of(listener, gl_surface, on_surface_destroy);

    // Release all buffers and clear the contents of the framebuffer.
    struct gl_buffer *gl_buffer, *tmp;
    wl_list_for_each_safe (gl_buffer, tmp, &gl_surface->buffers, link) {
        gl_buffer_destroy(gl_buffer);
    }

    eglMakeCurrent(gl_surface->gl->egl.display, gl_surface->egl_surface, gl_surface->egl_surface,
                   gl_surface->gl->egl.ctx);

    glClearColor(0.0, 0.0, 0.0, 0.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    eglSwapBuffers(gl_surface->gl->egl.display, gl_surface->egl_surface);
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
gl_buffer_import(struct server_gl_surface *gl_surface, struct server_buffer *buffer) {
    if (strcmp(buffer->impl->name, SERVER_BUFFER_DMABUF) != 0) {
        ww_log(LOG_ERROR, "cannot create server_gl_surface for non-DMABUF buffer");
        return NULL;
    }

    struct gl_buffer *gl_buffer = zalloc(1, sizeof(*gl_buffer));
    gl_buffer->gl = gl_surface->gl;

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

    wl_list_insert(&gl_surface->buffers, &gl_buffer->link);

    return gl_buffer;

fail_image_target:
fail_make_current:
    gl_buffer->gl->egl.DestroyImageKHR(gl_buffer->gl->egl.display, gl_buffer->image);

fail_create_orig:
    server_buffer_unref(gl_buffer->parent);
    free(gl_buffer);

    return NULL;
}

static void
gl_surface_update(struct server_gl_surface *gl_surface) {
    ww_assert(gl_surface->current);

    // TODO: Lock gl_surface->current->parent (explicit sync). I have no idea how this will work out
    // in practice, hopefully there's an EGL/OpenGL extension for it?

    // Create the data to place into the vertex buffer object. There need to be two triangles to
    // form a quad.
    struct vertex {
        float pos[2];
        float texcoord[2];
    };

    struct box *crop = &gl_surface->options.crop;
    const struct vertex tl = {{-1, 1}, {crop->x, crop->y}};
    const struct vertex tr = {{1, 1}, {crop->x + crop->width, crop->y}};
    const struct vertex bl = {{-1, -1}, {crop->x, crop->y + crop->height}};
    const struct vertex br = {{1, -1}, {crop->x + crop->width, crop->y + crop->height}};

    const struct vertex vertices[] = {tl, tr, bl, tr, bl, br};

    // Use OpenGL to draw a new frame with the contents of the latest surface commit.
    eglMakeCurrent(gl_surface->gl->egl.display, gl_surface->egl_surface, gl_surface->egl_surface,
                   gl_surface->gl->egl.ctx);

    glViewport(0, 0, gl_surface->options.width, gl_surface->options.height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glClearColor(0, 0, 0, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(gl_surface->gl->shader.prog);
    glBindBuffer(GL_ARRAY_BUFFER, gl_surface->gl->shader.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(struct vertex) * STATIC_ARRLEN(vertices), vertices,
                 GL_DYNAMIC_DRAW);
    glVertexAttribPointer(gl_surface->gl->shader.attrib_pos, 2, GL_FLOAT, GL_FALSE,
                          sizeof(struct vertex), 0);
    glVertexAttribPointer(gl_surface->gl->shader.attrib_texcoord, 2, GL_FLOAT, GL_FALSE,
                          sizeof(struct vertex), (const void *)(sizeof(float) * 2));

    int32_t width, height;
    server_buffer_get_size(gl_surface->current->parent, &width, &height);
    glUniform2f(gl_surface->gl->shader.uniform_size, width, height);

    glUniform4f(gl_surface->gl->shader.uniform_key_src, gl_surface->options.src_rgba[0],
                gl_surface->options.src_rgba[1], gl_surface->options.src_rgba[2],
                gl_surface->options.src_rgba[3]);
    glUniform4f(gl_surface->gl->shader.uniform_key_dst, gl_surface->options.dst_rgba[0],
                gl_surface->options.dst_rgba[1], gl_surface->options.dst_rgba[2],
                gl_surface->options.dst_rgba[3]);

    glEnableVertexAttribArray(gl_surface->gl->shader.attrib_pos);
    glEnableVertexAttribArray(gl_surface->gl->shader.attrib_texcoord);
    glBindTexture(GL_TEXTURE_2D, gl_surface->current->texture);
    glDrawArrays(GL_TRIANGLES, 0, STATIC_ARRLEN(vertices));
    glDisableVertexAttribArray(gl_surface->gl->shader.attrib_pos);
    glDisableVertexAttribArray(gl_surface->gl->shader.attrib_texcoord);

    // Commit the new frame.
    eglSwapBuffers(gl_surface->gl->egl.display, gl_surface->egl_surface);
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

    // Create the shader program.
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

    gl->shader.attrib_texcoord = glGetAttribLocation(gl->shader.prog, "v_texcoord");
    ww_assert(gl->shader.attrib_texcoord != -1);

    gl->shader.uniform_size = glGetUniformLocation(gl->shader.prog, "u_size");
    ww_assert(gl->shader.uniform_size != -1);

    gl->shader.uniform_key_src = glGetUniformLocation(gl->shader.prog, "u_colorkey_src");
    ww_assert(gl->shader.uniform_key_src != -1);

    gl->shader.uniform_key_dst = glGetUniformLocation(gl->shader.prog, "u_colorkey_dst");
    ww_assert(gl->shader.uniform_key_dst != -1);

    glGenBuffers(1, &gl->shader.vbo);

    // Log debug information about the user's EGL/OpenGL implementation.
    egl_print_sysinfo(gl);

    return gl;

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
    // Destroy OpenGL resources.
    eglMakeCurrent(gl->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, gl->egl.ctx);

    glDeleteBuffers(1, &gl->shader.vbo);
    glDeleteProgram(gl->shader.prog);
    glDeleteShader(gl->shader.frag);
    glDeleteShader(gl->shader.vert);

    // Destroy EGL resources.
    eglMakeCurrent(gl->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(gl->egl.display, gl->egl.ctx);
    eglTerminate(gl->egl.display);

    free(gl);
}

struct server_gl_surface *
server_gl_surface_create(struct server_gl *gl, struct server_surface *surface,
                         struct server_gl_surface_options options) {
    struct server_gl_surface *gl_surface = zalloc(1, sizeof(*gl_surface));

    gl_surface->gl = gl;
    gl_surface->parent = surface;
    gl_surface->options = options;

    gl_surface->remote = wl_compositor_create_surface(gl->server->backend->compositor);
    check_alloc(gl_surface->remote);

    struct wl_region *empty_region = wl_compositor_create_region(gl->server->backend->compositor);
    check_alloc(empty_region);

    wl_surface_set_input_region(gl_surface->remote, empty_region);
    wl_region_destroy(empty_region);

    gl_surface->window = wl_egl_window_create(gl_surface->remote, options.width, options.height);
    if (!gl_surface->window) {
        ww_log(LOG_ERROR, "failed to create wl_egl_window (%dx%d)", options.crop.width,
               options.crop.height);
        goto fail_window;
    }

    gl_surface->egl_surface = gl->egl.CreatePlatformWindowSurfaceEXT(
        gl->egl.display, gl->egl.config, gl_surface->window, NULL);

    // Mesa (and probably the NVIDIA driver as well, if I had to guess) will create and wait for
    // wl_surface.frame callbacks if the swap interval is non-zero.
    eglMakeCurrent(gl->egl.display, gl_surface->egl_surface, gl_surface->egl_surface, gl->egl.ctx);
    eglSwapInterval(gl->egl.display, 0);

    gl_surface->on_surface_commit.notify = on_gl_surface_commit;
    wl_signal_add(&gl_surface->parent->events.commit, &gl_surface->on_surface_commit);

    // TODO: I don't think this is really needed since surface destruction means we're shutting down
    // anyway?
    gl_surface->on_surface_destroy.notify = on_gl_surface_destroy;
    wl_signal_add(&gl_surface->parent->events.destroy, &gl_surface->on_surface_destroy);

    wl_list_init(&gl_surface->buffers);

    return gl_surface;

fail_window:
    wl_surface_destroy(gl_surface->remote);
    free(gl_surface);

    return NULL;
}

void
server_gl_surface_destroy(struct server_gl_surface *gl_surface) {
    struct gl_buffer *gl_buffer, *tmp;
    wl_list_for_each_safe (gl_buffer, tmp, &gl_surface->buffers, link) {
        gl_buffer_destroy(gl_buffer);
    }

    if (eglMakeCurrent(gl_surface->gl->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       gl_surface->gl->egl.ctx)) {
        eglDestroySurface(gl_surface->gl->egl.display, gl_surface->egl_surface);
        wl_egl_window_destroy(gl_surface->window);
    } else {
        ww_log_egl(LOG_ERROR, "failed to make EGL context current");
    }

    wl_surface_destroy(gl_surface->remote);

    wl_list_remove(&gl_surface->on_surface_commit.link);
    wl_list_remove(&gl_surface->on_surface_destroy.link);

    free(gl_surface);
}
