#include "server/gl.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "scene.h"
#include "server/backend.h"
#include "server/buffer.h"
#include "server/server.h"
#include "server/ui.h"
#include "server/wl_compositor.h"
#include "server/wp_linux_dmabuf.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
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

static constexpr uint64_t DRM_FORMAT_MOD_INVALID = 0xFFFFFFFFFFFFFFF;
static constexpr int MAX_CACHED_DMABUF = 2;

#define ww_log_egl(lvl, fmt, ...)                                                                  \
    util_log(lvl, "[%s:%d] " fmt ": %s", __FILE__, __LINE__, ##__VA_ARGS__, egl_strerror())

struct gl_buffer {
    struct wl_list link; // server_gl.capture.buffers
    struct server_gl *gl;

    struct server_buffer *parent;
    EGLImageKHR image; // imported DMABUF - must not be modified
    GLuint texture;    // must not be modified
};

// clang-format off
static constexpr EGLint CONFIG_ATTRIBUTES[] = {
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_RED_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_BLUE_SIZE, 8,
    EGL_ALPHA_SIZE, 8,
    EGL_STENCIL_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_NONE,
};

static constexpr EGLint CONTEXT_ATTRIBUTES[] = {
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

static void
on_surface_commit(struct wl_listener *listener, void *data) {
    struct server_gl *gl = wl_container_of(listener, gl, on_surface_commit);

    wl_signal_emit_mutable(&gl->events.frame, nullptr);

    struct server_buffer *buffer = server_surface_next_buffer(gl->capture.surface);
    if (!buffer) {
        gl->capture.current = nullptr;
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
        gl->capture.current = nullptr;
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

    gl->capture.current = nullptr;
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
        return nullptr;
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

    gl_buffer->image =
        gl_buffer->gl->egl.CreateImageKHR(gl_buffer->gl->egl.display, EGL_NO_CONTEXT,
                                          EGL_LINUX_DMA_BUF_EXT, nullptr, image_attributes);
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
    gl_using_texture(GL_TEXTURE_2D, gl_buffer->texture) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        gl_buffer->gl->egl.ImageTargetTexture2DOES(GL_TEXTURE_2D, gl_buffer->image);
        if (!gl_checkerr("failed to import texture")) {
            glBindTexture(GL_TEXTURE_2D, 0);
            goto fail_image_target;
        }
    }

    wl_list_insert(&gl->capture.buffers, &gl_buffer->link);

    return gl_buffer;

fail_image_target:
    glDeleteTextures(1, &gl_buffer->texture);
fail_make_current:
    gl_buffer->gl->egl.DestroyImageKHR(gl_buffer->gl->egl.display, gl_buffer->image);

fail_create_orig:
    server_buffer_unref(gl_buffer->parent);
    free(gl_buffer);

    return nullptr;
}

static bool
compile_shader(GLuint *out, GLint type, const char *source) {
    GLuint shader = glCreateShader(type);
    ww_assert(shader != 0);
    *out = shader;

    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint status;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == GL_TRUE) {
        return true;
    }

    char buf[4096] = {0};
    glGetShaderInfoLog(shader, STATIC_ARRLEN(buf) - 1, nullptr, buf);
    ww_log(LOG_ERROR, "failed to compile shader (type %d): %s", (int)type, buf);

    return false;
}

static bool
compile_program(GLuint *out, GLuint vert, GLuint frag) {
    GLuint prog = glCreateProgram();
    ww_assert(prog != 0);
    *out = prog;

    glAttachShader(prog, vert);
    glAttachShader(prog, frag);

    glBindAttribLocation(prog, SHADER_SRC_POS_ATTRIB_LOC, "v_src_pos");
    glBindAttribLocation(prog, SHADER_DST_POS_ATTRIB_LOC, "v_dst_pos");
    glBindAttribLocation(prog, SHADER_SRC_RGBA_ATTRIB_LOC, "v_src_rgba");
    glBindAttribLocation(prog, SHADER_DST_RGBA_ATTRIB_LOC, "v_dst_rgba");

    glLinkProgram(prog);

    GLint status;
    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (status == GL_TRUE) {
        return true;
    }

    char buf[4096] = {0};
    glGetProgramInfoLog(prog, STATIC_ARRLEN(buf) - 1, nullptr, buf);
    ww_log(LOG_ERROR, "failed to link shader program: %s", buf);

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

    gl->egl.display = gl->egl.GetPlatformDisplayEXT(EGL_PLATFORM_WAYLAND_EXT,
                                                    gl->server->backend->display, nullptr);
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

    // Create the OpenGL surface.
    gl->surface.remote = wl_compositor_create_surface(server->backend->compositor);
    check_alloc(gl->surface.remote);
    wl_surface_set_input_region(gl->surface.remote, server->ui->empty_region);

    gl->surface.subsurface = wl_subcompositor_get_subsurface(
        server->backend->subcompositor, gl->surface.remote, server->ui->tree.surface);
    check_alloc(gl->surface.subsurface);
    wl_subsurface_set_desync(gl->surface.subsurface);

    // Use arbitrary sizes here since the main UI window has not yet been sized.
    gl->surface.window = wl_egl_window_create(gl->surface.remote, 1, 1);
    check_alloc(gl->surface.window);

    gl->surface.egl = gl->egl.CreatePlatformWindowSurfaceEXT(gl->egl.display, gl->egl.config,
                                                             gl->surface.window, nullptr);
    if (gl->surface.egl == EGL_NO_SURFACE) {
        ww_log_egl(LOG_ERROR, "failed to create EGL surface");
        goto fail_egl_surface;
    }

    server_gl_with(gl, true) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glClearColor(0, 0, 0, 0);
        server_gl_swap_buffers(gl);
    }

    gl->on_ui_resize.notify = on_ui_resize;
    wl_signal_add(&server->ui->events.resize, &gl->on_ui_resize);

    // Log debug information about the user's EGL/OpenGL implementation.
    server_gl_with(gl, false) {
        egl_print_sysinfo(gl);
    }

    wl_list_init(&gl->capture.buffers);

    wl_signal_init(&gl->events.frame);

    return gl;

fail_egl_surface:
    wl_egl_window_destroy(gl->surface.window);
    wl_subsurface_destroy(gl->surface.subsurface);
    wl_surface_destroy(gl->surface.remote);

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

    return nullptr;
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

    // Destroy surface resources.
    wl_list_remove(&gl->on_ui_resize.link);

    eglDestroySurface(gl->egl.display, gl->surface.egl);
    wl_egl_window_destroy(gl->surface.window);
    wl_subsurface_destroy(gl->surface.subsurface);
    wl_surface_destroy(gl->surface.remote);

    // Destroy EGL resources.
    eglMakeCurrent(gl->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(gl->egl.display, gl->egl.ctx);
    eglTerminate(gl->egl.display);

    free(gl);
}

void
server_gl_enter(struct server_gl *gl, bool surface) {
    EGLSurface egl_surface = surface ? gl->surface.egl : EGL_NO_SURFACE;

    if (!eglMakeCurrent(gl->egl.display, egl_surface, egl_surface, gl->egl.ctx)) {
        ww_panic("failed to make EGL context current (surface: %s): %s", surface ? "yes" : "no",
                 egl_strerror());
    }
}

void
server_gl_exit(struct server_gl *gl) {
    if (!eglMakeCurrent(gl->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
        ww_panic("failed to exit EGL context: %s", egl_strerror());
    }
}

struct server_gl_shader *
server_gl_compile(struct server_gl *gl, const char *vert, const char *frag) {
    // The OpenGL context must be current.

    struct server_gl_shader *shader = zalloc(1, sizeof(*shader));

    if (!compile_shader(&shader->vert, GL_VERTEX_SHADER, vert)) {
        goto fail_vert;
    }
    if (!compile_shader(&shader->frag, GL_FRAGMENT_SHADER, frag)) {
        goto fail_frag;
    }
    if (!compile_program(&shader->program, shader->vert, shader->frag)) {
        goto fail_link;
    }

    return shader;

fail_link:
    glDeleteShader(shader->frag);

fail_frag:
    glDeleteShader(shader->vert);

fail_vert:
    free(shader);

    return nullptr;
}

GLuint
server_gl_get_capture(struct server_gl *gl) {
    if (!gl->capture.current) {
        return 0;
    }

    return gl->capture.current->texture;
}

void
server_gl_get_capture_size(struct server_gl *gl, int32_t *width, int32_t *height) {
    ww_assert(gl->capture.current);
    server_buffer_get_size(gl->capture.current->parent, width, height);
}

void
server_gl_set_capture(struct server_gl *gl, struct server_surface *surface) {
    if (gl->capture.surface) {
        ww_panic("cannot overwrite capture surface");
    }

    gl->capture.surface = surface;

    gl->on_surface_commit.notify = on_surface_commit;
    wl_signal_add(&surface->events.commit, &gl->on_surface_commit);

    gl->on_surface_destroy.notify = on_surface_destroy;
    wl_signal_add(&surface->events.destroy, &gl->on_surface_destroy);
}

void
server_gl_swap_buffers(struct server_gl *gl) {
    eglSwapInterval(gl->egl.display, 0);
    eglSwapBuffers(gl->egl.display, gl->surface.egl);
}

void
server_gl_shader_destroy(struct server_gl_shader *shader) {
    // The OpenGL context must be current.

    glDeleteProgram(shader->program);
    glDeleteShader(shader->frag);
    glDeleteShader(shader->vert);

    free(shader);
}

void
server_gl_shader_use(struct server_gl_shader *shader) {
    // The OpenGL context must be current.

    glUseProgram(shader->program);
}
