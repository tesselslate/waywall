#include "scene.h"
#include "server/gl.h"
#include "server/ui.h"
#include "util/alloc.h"
#include "util/debug.h"
#include "util/font.h"
#include "util/log.h"
#include "util/png.h"
#include "util/prelude.h"
#include <GLES2/gl2.h>
#include <spng.h>

static constexpr int PACKED_ATLAS_SIZE = 4096;
static constexpr int PACKED_ATLAS_WIDTH = 2048;
static constexpr int PACKED_ATLAS_HEIGHT = 16;
static constexpr int ATLAS_WIDTH = 128;
static constexpr int ATLAS_HEIGHT = 256;
static constexpr int CHAR_WIDTH = 8;
static constexpr int CHAR_HEIGHT = 16;
static constexpr int CHARS_PER_ROW = (ATLAS_WIDTH / CHAR_WIDTH);

static_assert(PACKED_ATLAS_SIZE == STATIC_ARRLEN(UTIL_TERMINUS_FONT));
static_assert(PACKED_ATLAS_WIDTH * PACKED_ATLAS_HEIGHT == ATLAS_WIDTH * ATLAS_HEIGHT);
static_assert(ATLAS_WIDTH * ATLAS_HEIGHT == PACKED_ATLAS_SIZE * 8);

static constexpr char SHADER_FRAG_TEXCOPY[] = {
#embed "glsl/texcopy.frag"
    , 0};

static constexpr char SHADER_VERT_TEXCOPY[] = {
#embed "glsl/texcopy.vert"
    , 0};

struct vtx_shader {
    float src_pos[2];
    float dst_pos[2];
    float src_rgba[4];
    float dst_rgba[4];
};

enum scene_object_type {
    SCENE_OBJECT_IMAGE,
    SCENE_OBJECT_MIRROR,
    SCENE_OBJECT_TEXT,
};

struct scene_object {
    struct wl_list link;
    struct scene *parent;
    enum scene_object_type type;
    int32_t depth;
};

struct scene_image {
    struct scene_object object;
    struct scene *parent;

    size_t shader_index;

    GLuint tex, vbo;

    int32_t width, height;
};

struct scene_mirror {
    struct scene_object object;
    struct scene *parent;

    size_t shader_index;

    GLuint vbo;

    float src_rgba[4], dst_rgba[4];
};

struct scene_text {
    struct scene_object object;
    struct scene *parent;

    size_t shader_index;

    GLuint vbo;
    size_t vtxcount;

    int32_t x, y;
};

static void object_add(struct scene *scene, struct scene_object *object,
                       enum scene_object_type type);
static void object_list_destroy(struct wl_list *list);
static void object_release(struct scene_object *object);
static void object_render(struct scene_object *object);
static void object_sort(struct scene *scene, struct scene_object *object);

static void draw_debug_text(struct scene *scene);
static void draw_frame(struct scene *scene);
static void draw_vertex_list(struct scene_shader *shader, size_t num_vertices);
static void rect_build(struct vtx_shader out[static 6], const struct box *src,
                       const struct box *dst, const float src_rgba[static 4],
                       const float dst_rgba[static 4]);
static inline struct scene_image *scene_image_from_object(struct scene_object *object);
static inline struct scene_mirror *scene_mirror_from_object(struct scene_object *object);
static inline struct scene_text *scene_text_from_object(struct scene_object *object);

static void
image_build(struct scene_image *out, struct scene *scene, const struct scene_image_options *options,
            int32_t width, int32_t height) {
    struct vtx_shader vertices[6] = {};

    rect_build(vertices, &(struct box){0, 0, width, height}, &options->dst, (float[4]){0, 0, 0, 0},
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
image_release(struct scene_object *object) {
    struct scene_image *image = scene_image_from_object(object);

    if (image->parent) {
        server_gl_with(image->parent->gl, false) {
            glDeleteTextures(1, &image->tex);
            glDeleteBuffers(1, &image->vbo);
        }
    }

    image->parent = nullptr;
}

static void
image_render(struct scene_object *object) {
    // The OpenGL context must be current.
    struct scene_image *image = scene_image_from_object(object);
    struct scene *scene = image->parent;

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
mirror_build(struct scene_mirror *mirror, const struct scene_mirror_options *options,
             struct scene *scene) {
    struct vtx_shader vertices[6] = {};

    rect_build(vertices, &options->src, &options->dst, options->src_rgba, mirror->dst_rgba);

    server_gl_with(scene->gl, false) {
        glGenBuffers(1, &mirror->vbo);
        ww_assert(mirror->vbo != 0);

        gl_using_buffer(GL_ARRAY_BUFFER, mirror->vbo) {
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
        }
    }
}

static void
mirror_release(struct scene_object *object) {
    struct scene_mirror *mirror = scene_mirror_from_object(object);

    if (mirror->parent) {
        server_gl_with(mirror->parent->gl, false) {
            glDeleteBuffers(1, &mirror->vbo);
        }
    }

    mirror->parent = nullptr;
}

static void
mirror_render(struct scene_object *object) {
    // The OpenGL context must be current.

    struct scene_mirror *mirror = scene_mirror_from_object(object);
    struct scene *scene = mirror->parent;

    GLuint capture_texture = server_gl_get_capture(scene->gl);
    if (capture_texture == 0) {
        return;
    }

    int32_t width, height;
    server_gl_get_capture_size(scene->gl, &width, &height);

    server_gl_shader_use(scene->shaders.data[mirror->shader_index].shader);
    glUniform2f(scene->shaders.data[mirror->shader_index].shader_u_dst_size, scene->ui->width,
                scene->ui->height);
    glUniform2f(scene->shaders.data[mirror->shader_index].shader_u_src_size, width, height);

    gl_using_buffer(GL_ARRAY_BUFFER, mirror->vbo) {
        gl_using_texture(GL_TEXTURE_2D, capture_texture) {
            // Each mirror has 6 vertices in its vertex buffer.
            draw_vertex_list(&scene->shaders.data[mirror->shader_index], 6);
        }
    }
}

static size_t
text_build(GLuint vbo, struct scene *scene, const char *data,
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

        rect_build(ptr, &src, &dst, (float[4]){1.0, 1.0, 1.0, 1.0}, options->rgba);
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
text_release(struct scene_object *object) {
    struct scene_text *text = scene_text_from_object(object);

    if (text->parent) {
        server_gl_with(text->parent->gl, false) {
            glDeleteBuffers(1, &text->vbo);
        }
    }

    text->parent = nullptr;
}

static void
text_render(struct scene_object *object) {
    // The OpenGL context must be current.

    struct scene_text *text = scene_text_from_object(object);
    struct scene *scene = text->parent;

    server_gl_shader_use(scene->shaders.data[text->shader_index].shader);
    glUniform2f(scene->shaders.data[text->shader_index].shader_u_dst_size, scene->ui->width,
                scene->ui->height);
    glUniform2f(scene->shaders.data[text->shader_index].shader_u_src_size, ATLAS_WIDTH,
                ATLAS_HEIGHT);

    gl_using_buffer(GL_ARRAY_BUFFER, text->vbo) {
        gl_using_texture(GL_TEXTURE_2D, scene->buffers.font_tex) {
            draw_vertex_list(&scene->shaders.data[text->shader_index], text->vtxcount);
        }
    }
}

static void
on_gl_frame(struct wl_listener *listener, void *data) {
    struct scene *scene = wl_container_of(listener, scene, on_gl_frame);

    server_gl_with(scene->gl, true) {
        draw_frame(scene);
    }
}

static void
object_add(struct scene *scene, struct scene_object *object, enum scene_object_type type) {
    object->parent = scene;
    object->type = type;
    object_sort(scene, object);
}

static void
object_list_destroy(struct wl_list *list) {
    struct scene_object *object, *tmp;

    wl_list_for_each_safe (object, tmp, list, link) {
        wl_list_remove(&object->link);
        wl_list_init(&object->link);

        object_release(object);
    }
}

static void
object_release(struct scene_object *object) {
    switch (object->type) {
    case SCENE_OBJECT_IMAGE:
        image_release(object);
        break;
    case SCENE_OBJECT_MIRROR:
        mirror_release(object);
        break;
    case SCENE_OBJECT_TEXT:
        text_release(object);
        break;
    }
}

static void
object_render(struct scene_object *object) {
    switch (object->type) {
    case SCENE_OBJECT_IMAGE:
        image_render(object);
        break;
    case SCENE_OBJECT_MIRROR:
        mirror_render(object);
        break;
    case SCENE_OBJECT_TEXT:
        text_render(object);
        break;
    }
}

static void
object_sort(struct scene *scene, struct scene_object *object) {
    if (object->depth == 0) {
        switch (object->type) {
        case SCENE_OBJECT_IMAGE:
            wl_list_insert(&scene->objects.unsorted_images, &object->link);
            break;
        case SCENE_OBJECT_MIRROR:
            wl_list_insert(&scene->objects.unsorted_mirrors, &object->link);
            break;
        case SCENE_OBJECT_TEXT:
            wl_list_insert(&scene->objects.unsorted_text, &object->link);
            break;
        }

        return;
    }

    struct scene_object *needle = nullptr, *prev = nullptr;
    wl_list_for_each (needle, &scene->objects.sorted, link) {
        if (needle->depth >= object->depth) {
            break;
        }

        prev = needle;
    }

    if (prev) {
        wl_list_insert(&prev->link, &object->link);
    } else {
        wl_list_insert(&scene->objects.sorted, &object->link);
    }
}

static void
draw_stencil(struct scene *scene) {
    // The OpenGL context must be current.

    int32_t width, height;
    GLuint tex = server_gl_get_capture(scene->gl);
    if (tex == 0) {
        return;
    }
    server_gl_get_capture_size(scene->gl, &width, &height);

    // It would be possible to listen for resizes instead of checking whether the stencil buffer
    // needs an update every frame, but that would be more complicated and there is also no event
    // for the game being resized (as of writing this comment, at least.)
    //
    // TODO: Is it possible to avoid the frame of leeway? Would also be nice to avoid it in
    // draw_frame. This stuff really should be synchronized with the surface content. Might be worth
    // always doing compositing if scene objects are visible but I'm not very happy with that
    // solution.
    bool stencil_equal = scene->ui->width == scene->prev_frame.width &&
                         scene->ui->height == scene->prev_frame.height &&
                         width == scene->prev_frame.tex_width &&
                         height == scene->prev_frame.tex_height;
    if (stencil_equal) {
        scene->prev_frame.equal_frames++;
        if (scene->prev_frame.equal_frames > 1) {
            return;
        }
    }

    scene->prev_frame.width = scene->ui->width;
    scene->prev_frame.height = scene->ui->height;
    scene->prev_frame.tex_width = width;
    scene->prev_frame.tex_height = height;
    scene->prev_frame.equal_frames = 0;

    glClearStencil(0);
    glStencilMask(0xFF);
    glClear(GL_STENCIL_BUFFER_BIT);

    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);

    struct box dst = {
        .x = (scene->ui->width / 2) - (width / 2),
        .y = (scene->ui->height / 2) - (height / 2),
        .width = width,
        .height = height,
    };

    struct vtx_shader buf[6];
    rect_build(buf, &(struct box){0, 0, 1, 1}, &dst, (float[4]){}, (float[4]){});
    gl_using_buffer(GL_ARRAY_BUFFER, scene->buffers.stencil_rect) {
        gl_using_texture(GL_TEXTURE_2D, tex) {
            glBufferData(GL_ARRAY_BUFFER, sizeof(buf), buf, GL_STATIC_DRAW);
            server_gl_shader_use(scene->shaders.data[0].shader);
            draw_vertex_list(&scene->shaders.data[0], 6);
        }
    }

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glDisable(GL_STENCIL_TEST);
}

static void
draw_debug_text(struct scene *scene) {
    // The OpenGL context must be current.
    server_gl_shader_use(scene->shaders.data[0].shader);
    glUniform2f(scene->shaders.data[0].shader_u_dst_size, scene->ui->width, scene->ui->height);
    glUniform2f(scene->shaders.data[0].shader_u_src_size, ATLAS_WIDTH, ATLAS_HEIGHT);

    const char *str = util_debug_str();
    scene->buffers.debug_vtxcount = text_build(
        scene->buffers.debug, scene, str,
        &(struct scene_text_options){
            .x = 8, .y = 8, .rgba = {1, 1, 1, 1}, .size_multiplier = 1, .shader_name = nullptr});

    gl_using_buffer(GL_ARRAY_BUFFER, scene->buffers.debug) {
        gl_using_texture(GL_TEXTURE_2D, scene->buffers.font_tex) {
            draw_vertex_list(&scene->shaders.data[0], scene->buffers.debug_vtxcount);
        }
    }
}

static inline bool
should_draw_frame(struct scene *scene) {
    return util_debug_enabled || wl_list_length(&scene->objects.sorted) ||
           wl_list_length(&scene->objects.unsorted_text) ||
           wl_list_length(&scene->objects.unsorted_mirrors) ||
           wl_list_length(&scene->objects.unsorted_images);
}

static void
draw_frame(struct scene *scene) {
    // The OpenGL context must be current.

    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glViewport(0, 0, scene->ui->width, scene->ui->height);

    if (!should_draw_frame(scene)) {
        // TODO: Scene rendering could potentially be synchronized with the game subsurface (or
        // waywall could implement basic compositing) to avoid the need for this. Committing the
        // scene subsurface and game subsurface at separate times tends to break VRR, so not
        // committing new blank frames to the scene subsurface when there is nothing to draw is an
        // easy workaround to get VRR to work most of the time.
        //
        // HACK: It should only be necessary to draw and commit one blank frame before pausing the
        // drawing of new frames. However, in some unknown circumstances, Hyprland seems to never
        // render the last blank frame that gets committed, leaving the last drawn frame of the
        // scene visible. Rendering two blank frames appears to solve the issue.
        scene->skipped_frames++;
        if (scene->skipped_frames > 2) {
            return;
        }
    } else {
        scene->skipped_frames = 0;
    }

    draw_stencil(scene);
    glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);

    struct scene_object *object;
    struct wl_list *positive_depth = nullptr;
    glEnable(GL_STENCIL_TEST);
    wl_list_for_each (object, &scene->objects.sorted, link) {
        if (object->depth >= 0) {
            positive_depth = object->link.prev;
            break;
        }

        object_render(object);
    }
    glDisable(GL_STENCIL_TEST);

    wl_list_for_each (object, &scene->objects.unsorted_mirrors, link) {
        mirror_render(object);
    }
    wl_list_for_each (object, &scene->objects.unsorted_images, link) {
        image_render(object);
    }
    wl_list_for_each (object, &scene->objects.unsorted_text, link) {
        text_render(object);
    }
    if (positive_depth) {
        wl_list_for_each (object, positive_depth, link) {
            object_render(object);
        }
    }

    if (util_debug_enabled) {
        draw_debug_text(scene);
    }

    glUseProgram(0);
    server_gl_swap_buffers(scene->gl);
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

static void
rect_build(struct vtx_shader out[static 6], const struct box *s, const struct box *d,
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

static inline struct scene_image *
scene_image_from_object(struct scene_object *object) {
    ww_assert(object->type == SCENE_OBJECT_IMAGE);
    return (struct scene_image *)object;
}

static inline struct scene_mirror *
scene_mirror_from_object(struct scene_object *object) {
    ww_assert(object->type == SCENE_OBJECT_MIRROR);
    return (struct scene_mirror *)object;
}

static inline struct scene_text *
scene_text_from_object(struct scene_object *object) {
    ww_assert(object->type == SCENE_OBJECT_TEXT);
    return (struct scene_text *)object;
}

static bool
image_load(struct scene_image *out, struct scene *scene, const char *path) {
    struct util_png png = util_png_decode(path, scene->image_max_size);
    if (!png.data) {
        return false;
    }

    out->width = png.width;
    out->height = png.height;

    // Upload the decoded image data to a new OpenGL texture.
    server_gl_with(scene->gl, false) {
        glGenTextures(1, &out->tex);
        gl_using_texture(GL_TEXTURE_2D, out->tex) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, png.width, png.height, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, png.data);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
    }

    free(png.data);
    return true;
}

static int
shader_find_index(struct scene *scene, const char *key) {
    if (key == nullptr) {
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
    data->shader = server_gl_compile(gl, vertex ? vertex : SHADER_VERT_TEXCOPY,
                                     fragment ? fragment : SHADER_FRAG_TEXCOPY);
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
        ww_log(LOG_INFO, "max image size: %" PRIu32 "x%" PRIu32, scene->image_max_size,
               scene->image_max_size);

        scene->shaders.count = cfg->shaders.count + 1;
        scene->shaders.data = malloc(sizeof(struct scene_shader) * scene->shaders.count);
        if (!shader_create(scene->gl, &scene->shaders.data[0], strdup("default"), nullptr,
                           nullptr)) {
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
        glGenBuffers(1, &scene->buffers.stencil_rect);

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

    wl_list_init(&scene->objects.sorted);
    wl_list_init(&scene->objects.unsorted_images);
    wl_list_init(&scene->objects.unsorted_mirrors);
    wl_list_init(&scene->objects.unsorted_text);

    return scene;

fail_compile_texture_copy:
    free(scene);

    return nullptr;
}

void
scene_destroy(struct scene *scene) {
    object_list_destroy(&scene->objects.sorted);
    object_list_destroy(&scene->objects.unsorted_images);
    object_list_destroy(&scene->objects.unsorted_mirrors);
    object_list_destroy(&scene->objects.unsorted_text);

    server_gl_with(scene->gl, false) {
        for (size_t i = 0; i < scene->shaders.count; i++) {
            server_gl_shader_destroy(scene->shaders.data[i].shader);
            free(scene->shaders.data[i].name);
        }

        glDeleteBuffers(2, (GLuint[]){scene->buffers.debug, scene->buffers.stencil_rect});
        glDeleteTextures(1, &scene->buffers.font_tex);
    }
    free(scene->shaders.data);

    wl_list_remove(&scene->on_gl_frame.link);

    free(scene);
}

struct scene_image *
scene_add_image(struct scene *scene, const struct scene_image_options *options, const char *path) {
    struct scene_image *image = zalloc(1, sizeof(*image));

    image->parent = scene;

    // Load the PNG into an OpenGL texture.
    if (!image_load(image, scene, path)) {
        free(image);
        return nullptr;
    }

    // Find correct shader for this image
    image->shader_index = shader_find_index(scene, options->shader_name);

    // Build a vertex buffer containing the data for this image.
    image_build(image, scene, options, image->width, image->height);

    image->object.depth = options->depth;
    object_add(scene, (struct scene_object *)image, SCENE_OBJECT_IMAGE);

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

    mirror_build(mirror, options, scene);

    mirror->object.depth = options->depth;
    object_add(scene, (struct scene_object *)mirror, SCENE_OBJECT_MIRROR);

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

    server_gl_with(scene->gl, false) {
        glGenBuffers(1, &text->vbo);
        ww_assert(text->vbo);

        text->vtxcount = text_build(text->vbo, scene, data, options);
    }

    text->object.depth = options->depth;
    object_add(scene, (struct scene_object *)text, SCENE_OBJECT_TEXT);

    return text;
}

void
scene_object_destroy(struct scene_object *object) {
    wl_list_remove(&object->link);
    wl_list_init(&object->link);

    object_release(object);
    free(object);
}

int32_t
scene_object_get_depth(struct scene_object *object) {
    return object->depth;
}

void
scene_object_set_depth(struct scene_object *object, int32_t depth) {
    if (depth == object->depth) {
        return;
    }

    object->depth = depth;
    wl_list_remove(&object->link);
    object_sort(object->parent, object);
}
