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

// clang-format off
// There appears to be a bug in clang-format which causes it to remove the space after some
// of these asterisks

static_assert(PACKED_ATLAS_WIDTH * PACKED_ATLAS_HEIGHT == ATLAS_WIDTH * ATLAS_HEIGHT);
static_assert(ATLAS_WIDTH * ATLAS_HEIGHT == PACKED_ATLAS_SIZE * 8);

// clang-format on

struct vtx_shader {
    float src_pos[2];
    float dst_pos[2];
    float src_rgba[4];
    float dst_rgba[4];
};

struct scene_image {
    struct wl_list link; // scene.images
    struct scene *parent;

    size_t shader_index;

    GLuint tex, vbo;

    int32_t width, height;
};

struct scene_mirror {
    struct wl_list link; // scene.mirrors
    struct scene *parent;

    size_t shader_index;

    GLuint vbo;

    float src_rgba[4], dst_rgba[4];
};

struct scene_text {
    struct wl_list link; // scene.text
    struct scene *parent;

    size_t shader_index;

    GLuint vbo;
    size_t vtxcount;

    int32_t x, y;
};

static void build_image(struct scene_image *out, struct scene *scene,
                        const struct scene_image_options *options, int32_t width, int32_t height);
static void build_mirror(struct scene_mirror *mirror, const struct scene_mirror_options *options,
                         struct scene *scene);
static void build_rect(struct vtx_shader out[static 6], const struct box *src,
                       const struct box *dst, const float src_rgba[static 4],
                       const float dst_rgba[static 4]);
static size_t build_text(GLuint vbo, struct scene *scene, const char *data,
                         const struct scene_text_options *options);

static void draw_debug_text(struct scene *scene);
static void draw_frame(struct scene *scene);
static void draw_image(struct scene *scene, struct scene_image *image);
static void draw_mirror(struct scene *scene, struct scene_mirror *mirror,
                        struct server_gl_buffer *capture, int32_t width, int32_t height);
static void draw_text(struct scene *scene, struct scene_text *text);

static void draw_vertex_list(struct scene_shader *shader, size_t num_vertices);

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
    struct vtx_shader vertices[6] = {0};

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
build_mirror(struct scene_mirror *mirror, const struct scene_mirror_options *options,
             struct scene *scene) {
    struct vtx_shader vertices[6] = {0};

    build_rect(vertices, &options->src, &options->dst, options->src_rgba, mirror->dst_rgba);

    server_gl_with(scene->gl, false) {
        glGenBuffers(1, &mirror->vbo);
        ww_assert(mirror->vbo != 0);

        gl_using_buffer(GL_ARRAY_BUFFER, mirror->vbo) {
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
        }
    }
}

static void
build_rect(struct vtx_shader out[static 6], const struct box *s, const struct box *d,
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
        struct vtx_shader *vtx = &out[i];

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

    struct vtx_shader *vertices = zalloc(vtxcount, sizeof(*vertices));
    struct vtx_shader *ptr = vertices;

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
    // The OpenGL context must be current,
    server_gl_shader_use(scene->shaders.data[0].shader);
    glUniform2f(scene->shaders.data[0].shader_u_dst_size, scene->ui->width, scene->ui->height);
    glUniform2f(scene->shaders.data[0].shader_u_src_size, ATLAS_WIDTH, ATLAS_HEIGHT);

    const char *str = util_debug_str();
    scene->buffers.debug_vtxcount = build_text(
        scene->buffers.debug, scene, str,
        &(struct scene_text_options){
            .x = 8, .y = 8, .rgba = {1, 1, 1, 1}, .size_multiplier = 1, .shader_name = NULL});

    gl_using_buffer(GL_ARRAY_BUFFER, scene->buffers.debug) {
        draw_vertex_list(&scene->shaders.data[0], scene->buffers.debug_vtxcount);
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

    struct server_gl_buffer *capture = server_gl_get_capture(scene->gl);
    bool have_mirrors = (!!capture && !wl_list_empty(&scene->mirrors));
    bool have_images = !wl_list_empty(&scene->images);
    bool have_text = (util_debug_enabled || !wl_list_empty(&scene->text));

    if (!have_mirrors && !have_images && !have_text) {
        scene->skipped_frames++;

        if (scene->skipped_frames > 1) {
            return;
        }
    } else {
        scene->skipped_frames = 0;
    }

    // Draw all mirrors using their respective shaders.
    if (capture) {
        int32_t width, height;
        server_gl_get_capture_size(scene->gl, &width, &height);
        struct scene_mirror *mirror;
        wl_list_for_each (mirror, &scene->mirrors, link) {
            draw_mirror(scene, mirror, capture, width, height);
        }
    }

    // Draw all images using their respective shaders.
    struct scene_image *image;
    wl_list_for_each (image, &scene->images, link) {
        draw_image(scene, image);
    }

    // Draw all text using their respective shaders.
    gl_using_texture(GL_TEXTURE_2D, scene->buffers.font_tex) {
        struct scene_text *text;
        wl_list_for_each (text, &scene->text, link) {
            draw_text(scene, text);
        }

        if (util_debug_enabled) {
            draw_debug_text(scene);
        }
    }

    glUseProgram(0);
    server_gl_swap_buffers(scene->gl);
}

static void
draw_image(struct scene *scene, struct scene_image *image) {
    // The OpenGL context must be current.
    server_gl_shader_use(scene->shaders.data[image->shader_index].shader);
    glUniform2f(scene->shaders.data[image->shader_index].shader_u_dst_size, scene->ui->width,
                scene->ui->height);
    glUniform2f(scene->shaders.data[image->shader_index].shader_u_src_size, image->width,
                image->height);

    gl_using_buffer(GL_ARRAY_BUFFER, image->vbo) {
        gl_using_texture(GL_TEXTURE_2D, image->tex) {
            // Each image has 6 vertices in its vertex buffer.
            draw_vertex_list(&scene->shaders.data[image->shader_index], 6);
        }
    }
}

static void
draw_mirror(struct scene *scene, struct scene_mirror *mirror, struct server_gl_buffer *capture,
            int32_t width, int32_t height) {
    // The OpenGL context must be current.
    server_gl_shader_use(scene->shaders.data[mirror->shader_index].shader);
    glUniform2f(scene->shaders.data[mirror->shader_index].shader_u_dst_size, scene->ui->width,
                scene->ui->height);
    glUniform2f(scene->shaders.data[mirror->shader_index].shader_u_src_size, width, height);

    GLuint target = server_gl_buffer_get_target(capture);
    GLuint texture = server_gl_buffer_get_texture(capture);

    gl_using_buffer(GL_ARRAY_BUFFER, mirror->vbo) {
        gl_using_texture(target, texture) {
            // Each mirror has 6 vertices in its vertex buffer.
            draw_vertex_list(&scene->shaders.data[mirror->shader_index], 6);
        }
    }
}

static void
draw_text(struct scene *scene, struct scene_text *text) {
    // The OpenGL context must be current.
    server_gl_shader_use(scene->shaders.data[text->shader_index].shader);
    glUniform2f(scene->shaders.data[text->shader_index].shader_u_dst_size, scene->ui->width,
                scene->ui->height);
    glUniform2f(scene->shaders.data[text->shader_index].shader_u_src_size, ATLAS_WIDTH,
                ATLAS_HEIGHT);

    gl_using_buffer(GL_ARRAY_BUFFER, text->vbo) {
        draw_vertex_list(&scene->shaders.data[text->shader_index], text->vtxcount);
    }
}

static void
draw_vertex_list(struct scene_shader *shader, size_t num_vertices) {
    // The OpenGL context must be current, a texture must be bound to copy from, a vertex buffer
    // with data must be bound, and a valid shader must be in use.

    glVertexAttribPointer(SHADER_SRC_POS_ATTRIB_LOC, 2, GL_FLOAT, GL_FALSE,
                          sizeof(struct vtx_shader),
                          (const void *)offsetof(struct vtx_shader, src_pos));
    glVertexAttribPointer(SHADER_DST_POS_ATTRIB_LOC, 2, GL_FLOAT, GL_FALSE,
                          sizeof(struct vtx_shader),
                          (const void *)offsetof(struct vtx_shader, dst_pos));
    glVertexAttribPointer(SHADER_SRC_RGBA_ATTRIB_LOC, 4, GL_FLOAT, GL_FALSE,
                          sizeof(struct vtx_shader),
                          (const void *)offsetof(struct vtx_shader, src_rgba));
    glVertexAttribPointer(SHADER_DST_RGBA_ATTRIB_LOC, 4, GL_FLOAT, GL_FALSE,
                          sizeof(struct vtx_shader),
                          (const void *)offsetof(struct vtx_shader, dst_rgba));

    glEnableVertexAttribArray(SHADER_SRC_POS_ATTRIB_LOC);
    glEnableVertexAttribArray(SHADER_DST_POS_ATTRIB_LOC);
    glEnableVertexAttribArray(SHADER_SRC_RGBA_ATTRIB_LOC);
    glEnableVertexAttribArray(SHADER_DST_RGBA_ATTRIB_LOC);

    glDrawArrays(GL_TRIANGLES, 0, num_vertices);

    glDisableVertexAttribArray(SHADER_SRC_POS_ATTRIB_LOC);
    glDisableVertexAttribArray(SHADER_DST_POS_ATTRIB_LOC);
    glDisableVertexAttribArray(SHADER_SRC_RGBA_ATTRIB_LOC);
    glDisableVertexAttribArray(SHADER_DST_RGBA_ATTRIB_LOC);
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
        server_gl_with(mirror->parent->gl, false) {
            glDeleteBuffers(1, &mirror->vbo);
        }
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

static int
shader_find_index(struct scene *scene, const char *key) {
    if (key == NULL) {
        return 0;
    }
    for (size_t i = 1; i < scene->shaders.count; i++) {
        if (strcmp(scene->shaders.data[i].name, key) == 0) {
            return i;
        }
    }
    ww_log(LOG_WARN, "shader %s not found, falling back to default", key);
    return 0;
}

static bool
shader_create(struct server_gl *gl, struct scene_shader *data, char *name, const char *vertex,
              const char *fragment) {
    data->name = name;
    data->shader = server_gl_compile(gl, vertex ? vertex : WAYWALL_GLSL_TEXCOPY_VERT_H,
                                     fragment ? fragment : WAYWALL_GLSL_TEXCOPY_FRAG_H);
    if (!data->shader) {
        return false;
    }

    data->shader_u_src_size = glGetUniformLocation(data->shader->program, "u_src_size");
    data->shader_u_dst_size = glGetUniformLocation(data->shader->program, "u_dst_size");

    return true;
}

struct scene *
scene_create(struct config *cfg, struct server_gl *gl, struct server_ui *ui) {
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

        scene->shaders.count = cfg->shaders.count + 1;
        scene->shaders.data = malloc(sizeof(struct scene_shader) * scene->shaders.count);
        if (!shader_create(scene->gl, &scene->shaders.data[0], strdup("default"), NULL, NULL)) {
            ww_log(LOG_ERROR, "error creating default shader");
            server_gl_exit(scene->gl);
            goto fail_compile_texture_copy;
        }
        for (size_t i = 0; i < cfg->shaders.count; i++) {
            if (!shader_create(scene->gl, &scene->shaders.data[i + 1],
                               strdup(cfg->shaders.data[i].name), cfg->shaders.data[i].vertex,
                               cfg->shaders.data[i].fragment)) {
                ww_log(LOG_ERROR, "error creating %s shader", cfg->shaders.data[i].name);
                server_gl_exit(scene->gl);
                goto fail_compile_texture_copy;
            }
            ww_log(LOG_INFO, "created %s shader", cfg->shaders.data[i].name);
        }

        // Initialize vertex buffers.
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
        for (size_t i = 0; i < scene->shaders.count; i++) {
            server_gl_shader_destroy(scene->shaders.data[i].shader);
            free(scene->shaders.data[i].name);
        }

        glDeleteBuffers(1, &scene->buffers.debug);
        glDeleteTextures(1, &scene->buffers.font_tex);
    }
    free(scene->shaders.data);

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

    // Find correct shader for this image
    image->shader_index = shader_find_index(scene, options->shader_name);

    // Build a vertex buffer containing the data for this image.
    build_image(image, scene, options, image->width, image->height);

    wl_list_insert(&scene->images, &image->link);

    return image;
}

struct scene_mirror *
scene_add_mirror(struct scene *scene, const struct scene_mirror_options *options) {
    struct scene_mirror *mirror = zalloc(1, sizeof(*mirror));

    mirror->parent = scene;
    memcpy(mirror->src_rgba, options->src_rgba, sizeof(mirror->src_rgba));
    memcpy(mirror->dst_rgba, options->dst_rgba, sizeof(mirror->dst_rgba));

    // Find correct shader for this mirror
    mirror->shader_index = shader_find_index(scene, options->shader_name);

    wl_list_insert(&scene->mirrors, &mirror->link);

    build_mirror(mirror, options, scene);

    return mirror;
}

struct scene_text *
scene_add_text(struct scene *scene, const char *data, const struct scene_text_options *options) {
    struct scene_text *text = zalloc(1, sizeof(*text));

    text->parent = scene;
    text->x = options->x;
    text->y = options->y;

    // Find correct shader for this text
    text->shader_index = shader_find_index(scene, options->shader_name);

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
