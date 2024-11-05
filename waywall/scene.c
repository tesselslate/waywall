#include "glsl/texcopy.frag.h"
#include "glsl/texcopy.vert.h"
#include "util/debug.h"

#include "scene.h"
#include "server/gl.h"
#include "server/ui.h"
#include "util/alloc.h"
#include "util/font.h"
#include "util/log.h"
#include "util/prelude.h"
#include <GLES2/gl2.h>
#include <spng.h>

#define PACKED_ATLAS_SIZE 4096
#define PACKED_ATLAS_WIDTH 2048
#define PACKED_ATLAS_HEIGHT 16
#define ATLAS_WIDTH 128
#define ATLAS_HEIGHT 256
#define CHAR_WIDTH 8
#define CHAR_HEIGHT 16
#define CHARS_PER_ROW (ATLAS_WIDTH / CHAR_WIDTH)

static_assert(PACKED_ATLAS_SIZE == STATIC_ARRLEN(UTIL_TERMINUS_FONT));
static_assert(PACKED_ATLAS_WIDTH * PACKED_ATLAS_HEIGHT == ATLAS_WIDTH * ATLAS_HEIGHT);
static_assert(ATLAS_WIDTH * ATLAS_HEIGHT == PACKED_ATLAS_SIZE * 8);

struct vtx_texcopy {
    float src_pos[2];
    float dst_pos[2];
    float src_rgba[4];
    float dst_rgba[4];
};

struct scene_image {
    struct wl_list link; // scene.images
    struct scene *parent;

    GLuint tex, vbo;

    int32_t width, height;
};

struct scene_mirror {
    struct wl_list link; // scene.mirrors
    struct scene *parent;

    struct scene_mirror_options options;
};

struct scene_text {
    struct wl_list link; // scene.text
    struct scene *parent;

    GLuint vbo;
    size_t vtxcount;

    int32_t x, y;
};

static void build_image(struct scene_image *out, struct scene *scene,
                        const struct scene_image_options *options, int32_t width, int32_t height);
static void build_mirrors(struct scene *scene);
static void build_rect(struct vtx_texcopy out[static 6], const struct box *src,
                       const struct box *dst, const float src_rgba[static 4],
                       const float dst_rgba[static 4]);
static size_t build_text(GLuint vbo, struct scene *scene, const char *data,
                         const struct scene_text_options *options);

static void draw_debug_text(struct scene *scene);
static void draw_frame(struct scene *scene);
static void draw_image(struct scene *scene, struct scene_image *image);
static void draw_mirrors(struct scene *scene);
static void draw_text(struct scene *scene, struct scene_text *text);

static void draw_texcopy_list(struct scene *scene, size_t num_vertices);

static void
on_gl_frame(struct wl_listener *listener, void *data) {
    struct scene *scene = wl_container_of(listener, scene, on_gl_frame);

    server_gl_with(scene->gl, true) {
        draw_frame(scene);
    }
}

static void
build_image(struct scene_image *out, struct scene *scene, const struct scene_image_options *options,
            int32_t width, int32_t height) {
    struct vtx_texcopy vertices[6] = {0};

    build_rect(vertices, &(struct box){0, 0, width, height}, &options->dst, (float[4]){0, 0, 0, 0},
               (float[4]){0, 0, 0, 0});

    server_gl_with(scene->gl, false) {
        glGenBuffers(1, &out->vbo);
        ww_assert(out->vbo != 0);

        gl_using_buffer(GL_ARRAY_BUFFER, out->vbo) {
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        }
    }
}

static void
build_mirrors(struct scene *scene) {
    scene->buffers.mirrors_vtxcount = wl_list_length(&scene->mirrors) * 6;

    struct vtx_texcopy *vertices = zalloc(scene->buffers.mirrors_vtxcount, sizeof(*vertices));
    struct vtx_texcopy *ptr = vertices;

    struct scene_mirror *mirror;
    wl_list_for_each (mirror, &scene->mirrors, link) {
        build_rect(ptr, &mirror->options.src, &mirror->options.dst, mirror->options.src_rgba,
                   mirror->options.dst_rgba);

        ptr += 6;
    }

    server_gl_with(scene->gl, false) {
        gl_using_buffer(GL_ARRAY_BUFFER, scene->buffers.mirrors) {
            glBufferData(GL_ARRAY_BUFFER, scene->buffers.mirrors_vtxcount * sizeof(*vertices),
                         vertices, GL_STREAM_DRAW);
        }
    }

    free(vertices);
}

static void
build_rect(struct vtx_texcopy out[static 6], const struct box *s, const struct box *d,
           const float src_rgba[static 4], const float dst_rgba[static 4]) {
    const struct {
        float src[2];
        float dst[2];
    } data[] = {
        // top-left triangle
        {{s->x, s->y}, {d->x, d->y}},
        {{s->x + s->width, s->y}, {d->x + d->width, d->y}},
        {{s->x, s->y + s->height}, {d->x, d->y + d->height}},

        // bottom-right triangle
        {{s->x + s->width, s->y}, {d->x + d->width, d->y}},
        {{s->x, s->y + s->height}, {d->x, d->y + d->height}},
        {{s->x + s->width, s->y + s->height}, {d->x + d->width, d->y + d->height}},
    };

    for (size_t i = 0; i < STATIC_ARRLEN(data); i++) {
        struct vtx_texcopy *vtx = &out[i];

        memcpy(vtx->src_pos, data[i].src, sizeof(vtx->src_pos));
        memcpy(vtx->dst_pos, data[i].dst, sizeof(vtx->dst_pos));
        memcpy(vtx->src_rgba, src_rgba, sizeof(vtx->src_rgba));
        memcpy(vtx->dst_rgba, dst_rgba, sizeof(vtx->dst_rgba));
    }
}

static size_t
build_text(GLuint vbo, struct scene *scene, const char *data,
           const struct scene_text_options *options) {
    // The OpenGL context must be current.

    size_t vtxcount = strlen(data) * 6;

    struct vtx_texcopy *vertices = zalloc(vtxcount, sizeof(*vertices));
    struct vtx_texcopy *ptr = vertices;

    int32_t x = options->x;
    int32_t y = options->y;

    for (const char *c = data; *c != '\0'; c++) {
        if (*c == '\n') {
            y += CHAR_HEIGHT * options->size_multiplier;
            x = options->x;
            continue;
        } else if (*c == ' ') {
            x += CHAR_WIDTH * options->size_multiplier;
            continue;
        }

        struct box src = {
            .x = (*c % CHARS_PER_ROW) * CHAR_WIDTH,
            .y = (*c / CHARS_PER_ROW) * CHAR_HEIGHT,
            .width = CHAR_WIDTH,
            .height = CHAR_HEIGHT,
        };

        struct box dst = {
            .x = x,
            .y = y,
            .width = CHAR_WIDTH * options->size_multiplier,
            .height = CHAR_HEIGHT * options->size_multiplier,
        };

        build_rect(ptr, &src, &dst, (float[4]){1.0, 1.0, 1.0, 1.0}, options->rgba);
        ptr += 6;

        x += CHAR_WIDTH * options->size_multiplier;
    }

    gl_using_buffer(GL_ARRAY_BUFFER, vbo) {
        glBufferData(GL_ARRAY_BUFFER, vtxcount * sizeof(*vertices), vertices, GL_STATIC_DRAW);
    }

    free(vertices);

    return vtxcount;
}

static void
draw_debug_text(struct scene *scene) {
    // The OpenGL context must be current, the texcopy shader must be in use, and the font atlas
    // texture must be bound.

    const char *str = util_debug_str();
    scene->buffers.debug_vtxcount = build_text(
        scene->buffers.debug, scene, str,
        &(struct scene_text_options){.x = 8, .y = 8, .rgba = {1, 1, 1, 1}, .size_multiplier = 1});

    gl_using_buffer(GL_ARRAY_BUFFER, scene->buffers.debug) {
        draw_texcopy_list(scene, scene->buffers.debug_vtxcount);
    }
}

static void
draw_frame(struct scene *scene) {
    // The OpenGL context must be current.

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glClearColor(0, 0, 0, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glViewport(0, 0, scene->ui->width, scene->ui->height);

    // Use the texcopy shader.
    server_gl_shader_use(scene->shaders.texcopy);
    glUniform2f(scene->shaders.texcopy_u_dst_size, scene->ui->width, scene->ui->height);

    // Draw all mirrors using the texcopy shader.
    draw_mirrors(scene);

    // Draw all images using the texcopy shader.
    struct scene_image *image;
    wl_list_for_each (image, &scene->images, link) {
        draw_image(scene, image);
    }

    // Draw all text using the texcopy shader.
    gl_using_texture(GL_TEXTURE_2D, scene->buffers.font_tex) {
        glUniform2f(scene->shaders.texcopy_u_src_size, ATLAS_WIDTH, ATLAS_HEIGHT);

        struct scene_text *text;
        wl_list_for_each (text, &scene->text, link) {
            draw_text(scene, text);
        }

        if (util_debug_enabled) {
            draw_debug_text(scene);
        }
    }

    server_gl_swap_buffers(scene->gl);
}

static void
draw_image(struct scene *scene, struct scene_image *image) {
    // The OpenGL context must be current and the texcopy shader must be in use.

    glUniform2f(scene->shaders.texcopy_u_src_size, image->width, image->height);

    gl_using_buffer(GL_ARRAY_BUFFER, image->vbo) {
        gl_using_texture(GL_TEXTURE_2D, image->tex) {
            // Each image has 6 vertices in its vertex buffer.
            draw_texcopy_list(scene, 6);
        }
    }
}

static void
draw_mirrors(struct scene *scene) {
    // The OpenGL context must be current and the texcopy shader must be in use.

    unsigned int capture_texture = server_gl_get_capture(scene->gl);
    if (capture_texture == 0) {
        // There is no texture to capture. Do not draw any mirrors.
        return;
    }

    int32_t width, height;
    server_gl_get_capture_size(scene->gl, &width, &height);
    glUniform2f(scene->shaders.texcopy_u_src_size, width, height);

    gl_using_buffer(GL_ARRAY_BUFFER, scene->buffers.mirrors) {
        gl_using_texture(GL_TEXTURE_2D, capture_texture) {
            draw_texcopy_list(scene, scene->buffers.mirrors_vtxcount);
        }
    }
}

static void
draw_text(struct scene *scene, struct scene_text *text) {
    // The OpenGL context must be current, the texcopy shader must be in use, and the font atlas
    // texture must be bound.

    gl_using_buffer(GL_ARRAY_BUFFER, text->vbo) {
        draw_texcopy_list(scene, text->vtxcount);
    }
}

static void
draw_texcopy_list(struct scene *scene, size_t num_vertices) {
    // The OpenGL context must be current, a texture must be bound to copy from, a vertex buffer
    // with data must be bound, and the texcopy shader must be in use.

    glVertexAttribPointer(scene->shaders.texcopy_a_src_pos, 2, GL_FLOAT, GL_FALSE,
                          sizeof(struct vtx_texcopy),
                          (const void *)offsetof(struct vtx_texcopy, src_pos));
    glVertexAttribPointer(scene->shaders.texcopy_a_dst_pos, 2, GL_FLOAT, GL_FALSE,
                          sizeof(struct vtx_texcopy),
                          (const void *)offsetof(struct vtx_texcopy, dst_pos));
    glVertexAttribPointer(scene->shaders.texcopy_a_src_rgba, 4, GL_FLOAT, GL_FALSE,
                          sizeof(struct vtx_texcopy),
                          (const void *)offsetof(struct vtx_texcopy, src_rgba));
    glVertexAttribPointer(scene->shaders.texcopy_a_dst_rgba, 4, GL_FLOAT, GL_FALSE,
                          sizeof(struct vtx_texcopy),
                          (const void *)offsetof(struct vtx_texcopy, dst_rgba));

    glEnableVertexAttribArray(scene->shaders.texcopy_a_src_pos);
    glEnableVertexAttribArray(scene->shaders.texcopy_a_dst_pos);
    glEnableVertexAttribArray(scene->shaders.texcopy_a_src_rgba);
    glEnableVertexAttribArray(scene->shaders.texcopy_a_dst_rgba);

    glDrawArrays(GL_TRIANGLES, 0, num_vertices);

    glDisableVertexAttribArray(scene->shaders.texcopy_a_src_pos);
    glDisableVertexAttribArray(scene->shaders.texcopy_a_dst_pos);
    glDisableVertexAttribArray(scene->shaders.texcopy_a_src_rgba);
    glDisableVertexAttribArray(scene->shaders.texcopy_a_dst_rgba);
}

static bool
image_load(struct scene_image *out, struct scene *scene, void *pngbuf, size_t pngbuf_size) {
    int err;

    struct spng_ctx *ctx = spng_ctx_new(0);
    if (!ctx) {
        ww_log(LOG_ERROR, "failed to create spng context");
        return false;
    }
    spng_set_image_limits(ctx, scene->image_max_size, scene->image_max_size);

    // Decode the PNG.
    err = spng_set_png_buffer(ctx, pngbuf, pngbuf_size);
    if (err != 0) {
        ww_log(LOG_ERROR, "failed to set png buffer: %s\n", spng_strerror(err));
        goto fail_spng_set_png_buffer;
    }

    struct spng_ihdr ihdr;
    err = spng_get_ihdr(ctx, &ihdr);
    if (err != 0) {
        ww_log(LOG_ERROR, "failed to get image header: %s\n", spng_strerror(err));
        goto fail_spng_get_ihdr;
    }

    size_t decode_size;
    err = spng_decoded_image_size(ctx, SPNG_FMT_RGBA8, &decode_size);
    if (err != 0) {
        ww_log(LOG_ERROR, "failed to get image size: %s\n", spng_strerror(err));
        goto fail_spng_decoded_image_size;
    }

    char *decode_buf = malloc(decode_size);
    check_alloc(decode_buf);

    err = spng_decode_image(ctx, decode_buf, decode_size, SPNG_FMT_RGBA8, SPNG_DECODE_TRNS);
    if (err != 0) {
        ww_log(LOG_ERROR, "failed to decode image: %s\n", spng_strerror(err));
        goto fail_spng_decode_image;
    }

    out->width = ihdr.width;
    out->height = ihdr.height;

    // Upload the decoded image data to a new OpenGL texture.
    server_gl_with(scene->gl, false) {
        glGenTextures(1, &out->tex);
        gl_using_texture(GL_TEXTURE_2D, out->tex) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ihdr.width, ihdr.height, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, decode_buf);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
    }

    free(decode_buf);
    spng_ctx_free(ctx);

    return true;

fail_spng_decode_image:
    free(decode_buf);

fail_spng_decoded_image_size:
fail_spng_get_ihdr:
fail_spng_set_png_buffer:
    spng_ctx_free(ctx);

    return false;
}

static void
image_release(struct scene_image *image) {
    wl_list_remove(&image->link);
    wl_list_init(&image->link);

    if (image->parent) {
        server_gl_with(image->parent->gl, false) {
            glDeleteTextures(1, &image->tex);
            glDeleteBuffers(1, &image->vbo);
        }
    }

    image->parent = NULL;
}

static void
mirror_release(struct scene_mirror *mirror) {
    wl_list_remove(&mirror->link);
    wl_list_init(&mirror->link);

    if (mirror->parent) {
        build_mirrors(mirror->parent);
    }

    mirror->parent = NULL;
}

static void
text_release(struct scene_text *text) {
    wl_list_remove(&text->link);
    wl_list_init(&text->link);

    if (text->parent) {
        server_gl_with(text->parent->gl, false) {
            glDeleteBuffers(1, &text->vbo);
        }
    }

    text->parent = NULL;
}

struct scene *
scene_create(struct server_gl *gl, struct server_ui *ui) {
    struct scene *scene = zalloc(1, sizeof(*scene));

    scene->gl = gl;
    scene->ui = ui;

    // Initialize OpenGL resources.
    server_gl_with(scene->gl, false) {
        GLint tex_size;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &tex_size);

        scene->image_max_size = (uint32_t)tex_size;
        ww_log(LOG_INFO, "max image size: %" PRIu32 "x%" PRIu32 "\n", scene->image_max_size,
               scene->image_max_size);

        // Initialize the texcopy shader.
        scene->shaders.texcopy =
            server_gl_compile(scene->gl, WAYWALL_GLSL_TEXCOPY_VERT_H, WAYWALL_GLSL_TEXCOPY_FRAG_H);
        if (!scene->shaders.texcopy) {
            server_gl_exit(scene->gl);
            goto fail_compile_texture_copy;
        }
        scene->shaders.texcopy_u_src_size =
            glGetUniformLocation(scene->shaders.texcopy->program, "u_src_size");
        scene->shaders.texcopy_u_dst_size =
            glGetUniformLocation(scene->shaders.texcopy->program, "u_dst_size");
        scene->shaders.texcopy_a_src_pos =
            glGetAttribLocation(scene->shaders.texcopy->program, "v_src_pos");
        scene->shaders.texcopy_a_dst_pos =
            glGetAttribLocation(scene->shaders.texcopy->program, "v_dst_pos");
        scene->shaders.texcopy_a_src_rgba =
            glGetAttribLocation(scene->shaders.texcopy->program, "v_src_rgba");
        scene->shaders.texcopy_a_dst_rgba =
            glGetAttribLocation(scene->shaders.texcopy->program, "v_dst_rgba");

        // Initialize vertex buffers.
        glGenBuffers(1, &scene->buffers.mirrors);
        glGenBuffers(1, &scene->buffers.debug);

        // Initialize the font texture atlas.
        glGenTextures(1, &scene->buffers.font_tex);
        unsigned char *atlas = zalloc(1, PACKED_ATLAS_SIZE * 32);
        for (size_t py = 0; py < PACKED_ATLAS_HEIGHT; py++) {
            for (size_t px = 0; px < PACKED_ATLAS_WIDTH; px++) {
                size_t packed_pos = py * PACKED_ATLAS_WIDTH + px;
                bool set = UTIL_TERMINUS_FONT[packed_pos / 8] & (1 << (7 - packed_pos % 8));

                if (set) {
                    size_t x = px % ATLAS_WIDTH;
                    size_t y = py + (px / ATLAS_WIDTH) * CHAR_HEIGHT;
                    size_t pos = (y * ATLAS_WIDTH + x) * 4;

                    atlas[pos] = 0xFF;
                    atlas[pos + 1] = 0xFF;
                    atlas[pos + 2] = 0xFF;
                    atlas[pos + 3] = 0xFF;
                }
            }
        }

        gl_using_texture(GL_TEXTURE_2D, scene->buffers.font_tex) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ATLAS_WIDTH, ATLAS_HEIGHT, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, atlas);

            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }

        free(atlas);
    }

    scene->on_gl_frame.notify = on_gl_frame;
    wl_signal_add(&gl->events.frame, &scene->on_gl_frame);

    wl_list_init(&scene->images);
    wl_list_init(&scene->mirrors);
    wl_list_init(&scene->text);

    return scene;

fail_compile_texture_copy:
    free(scene);

    return NULL;
}

void
scene_destroy(struct scene *scene) {
    struct scene_image *image, *image_tmp;
    wl_list_for_each_safe (image, image_tmp, &scene->images, link) {
        image_release(image);
    }

    struct scene_mirror *mirror, *mirror_tmp;
    wl_list_for_each_safe (mirror, mirror_tmp, &scene->mirrors, link) {
        mirror_release(mirror);
    }

    struct scene_text *text, *text_tmp;
    wl_list_for_each_safe (text, text_tmp, &scene->text, link) {
        text_release(text);
    }

    server_gl_with(scene->gl, false) {
        server_gl_shader_destroy(scene->shaders.texcopy);

        glDeleteBuffers(1, &scene->buffers.mirrors);
        glDeleteBuffers(1, &scene->buffers.debug);
        glDeleteTextures(1, &scene->buffers.font_tex);
    }

    wl_list_remove(&scene->on_gl_frame.link);

    free(scene);
}

struct scene_image *
scene_add_image(struct scene *scene, const struct scene_image_options *options, void *pngbuf,
                size_t pngbuf_size) {
    struct scene_image *image = zalloc(1, sizeof(*image));

    image->parent = scene;

    // Load the PNG into an OpenGL texture.
    if (!image_load(image, scene, pngbuf, pngbuf_size)) {
        free(image);
        return NULL;
    }

    // Build a vertex buffer containing the data for this image.
    build_image(image, scene, options, image->width, image->height);

    wl_list_insert(&scene->images, &image->link);

    return image;
}

struct scene_mirror *
scene_add_mirror(struct scene *scene, const struct scene_mirror_options *options) {
    struct scene_mirror *mirror = zalloc(1, sizeof(*mirror));

    mirror->parent = scene;
    mirror->options = *options;

    wl_list_insert(&scene->mirrors, &mirror->link);

    build_mirrors(scene);

    return mirror;
}

struct scene_text *
scene_add_text(struct scene *scene, const char *data, const struct scene_text_options *options) {
    struct scene_text *text = zalloc(1, sizeof(*text));

    text->parent = scene;
    text->x = options->x;
    text->y = options->y;

    wl_list_insert(&scene->text, &text->link);

    server_gl_with(scene->gl, false) {
        glGenBuffers(1, &text->vbo);
        ww_assert(text->vbo);

        text->vtxcount = build_text(text->vbo, scene, data, options);
    }

    return text;
}

void
scene_image_destroy(struct scene_image *image) {
    image_release(image);
    free(image);
}

void
scene_mirror_destroy(struct scene_mirror *mirror) {
    mirror_release(mirror);
    free(mirror);
}

void
scene_text_destroy(struct scene_text *text) {
    text_release(text);
    free(text);
}
