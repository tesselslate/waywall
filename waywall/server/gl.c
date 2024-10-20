#include "server/gl.h"
#include "server/backend.h"
#include "server/server.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <wayland-client-core.h>

/*
 * This code is partially based off of wlroots and hello-wayland:
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

#define ww_log_egl(lvl, fmt, ...)                                                                  \
    util_log(lvl, "[%s:%d] " fmt ": %s", __FILE__, __LINE__, ##__VA_ARGS__, egl_strerror())

// clang-format off
static const EGLint CONFIG_ATTRIBUTES[] = {
    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
    EGL_RED_SIZE, 1,
    EGL_GREEN_SIZE, 1,
    EGL_BLUE_SIZE, 1,
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
    "EGL_KHR_image_base",
    "EGL_MESA_image_dma_buf_export",
};

static const char *REQUIRED_GL_EXTENSIONS[] = {
    "GL_OES_EGL_image",
};

static const char *egl_strerror();

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

struct server_gl *
server_gl_create(struct server *server) {
    struct server_gl *gl = zalloc(1, sizeof(*gl));

    gl->server = server;


    // Initialize the EGL display.
    if (!egl_getproc(&gl->egl.GetPlatformDisplayEXT, "eglGetPlatformDisplayEXT")) {
        goto fail_get_proc_display;
    }

    gl->egl.display = gl->egl.GetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
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
    if (!egl_getproc(&gl->egl.ExportDMABUFImageMESA, "eglExportDMABUFImageMESA")) {
        goto fail_extensions_egl;
    }
    if (!egl_getproc(&gl->egl.ExportDMABUFImageQueryMESA, "eglExportDMABUFImageQueryMESA")) {
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

    // Log debug information about the user's EGL/OpenGL implementation.
    egl_print_sysinfo(gl);

    return gl;

fail_extensions_gl:
fail_make_current:
    eglDestroyContext(gl->egl.display, gl->egl.ctx);

fail_create_context:
fail_choose_config:
fail_extensions_egl:
    eglTerminate(gl->egl.display);

fail_initialize:
fail_get_display:
fail_get_proc_display:
    free(gl);

    return NULL;
}

void
server_gl_destroy(struct server_gl *gl) {
    eglMakeCurrent(gl->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (gl->egl.ctx) {
        eglDestroyContext(gl->egl.display, gl->egl.ctx);
    }

    eglTerminate(gl->egl.display);

    free(gl);
}
