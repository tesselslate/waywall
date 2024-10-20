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
 * This code (mostly EGL initialization) is based off of emersion's hello-wayland example:
 * https://github.com/emersion/hello-wayland/
 *
 * ============================================================================
 *
 * MIT License
 *
 * Copyright (c) 2018 emersion
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define ww_log_egl(lvl, fmt, ...)                                                                  \
    util_log(lvl, "[%s:%d] " fmt ": %s", __FILE__, __LINE__, ##__VA_ARGS__, str_eglerror())

static const char *
str_eglerror() {
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
    // clang-format off
    static const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 1,
        EGL_GREEN_SIZE, 1,
        EGL_BLUE_SIZE, 1,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE,
    };

    static const EGLint context_attributes[] = {
        EGL_CONTEXT_MAJOR_VERSION, 2,
        EGL_NONE,
    };
    // clang-format on

    struct server_gl *gl = zalloc(1, sizeof(*gl));

    gl->server = server;

    const struct {
        void **out;
        const char *name;
    } egl_functions[] = {
        {(void *)&gl->egl.GetPlatformDisplayEXT, "eglGetPlatformDisplayEXT"},
    };

    for (size_t i = 0; i < STATIC_ARRLEN(egl_functions); i++) {
        *egl_functions[i].out = (void *)eglGetProcAddress(egl_functions[i].name);
        if (!egl_functions[i].out) {
            ww_log(LOG_ERROR, "EGL function '%s' does not exist", egl_functions[i].name);
            goto fail_egl_get_proc;
        }
    }

    gl->egl.display = gl->egl.GetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
    if (gl->egl.display == EGL_NO_DISPLAY) {
        ww_log(LOG_ERROR, "no EGL display available");
        goto fail_egl_get_display;
    }

    if (!eglInitialize(gl->egl.display, &gl->egl.major, &gl->egl.minor)) {
        ww_log_egl(LOG_ERROR, "failed to initialize EGL");
        goto fail_egl_initialize;
    }
    EGLint n = 0;
    if (!eglChooseConfig(gl->egl.display, config_attributes, &gl->egl.config, 1, &n) || n == 0) {
        ww_log_egl(LOG_ERROR, "failed to choose EGL config");
        goto fail_egl_choose_config;
    }

    gl->egl.ctx =
        eglCreateContext(gl->egl.display, gl->egl.config, EGL_NO_CONTEXT, context_attributes);
    if (gl->egl.ctx == EGL_NO_CONTEXT) {
        ww_log(LOG_ERROR, "failed to create EGL context");
        goto fail_egl_create_context;
    }

    return gl;

fail_egl_create_context:
fail_egl_choose_config:
    eglTerminate(gl->egl.display);

fail_egl_initialize:
fail_egl_get_display:
fail_egl_get_proc:
    free(gl);
    return NULL;
}

void
server_gl_destroy(struct server_gl *gl) {
    if (gl->egl.ctx) {
        eglDestroyContext(gl->egl.display, gl->egl.ctx);
    }

    eglTerminate(gl->egl.display);

    free(gl);
}
