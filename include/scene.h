#ifndef WAYWALL_SCENE_H
#define WAYWALL_SCENE_H

#include "config/config.h"
#include "util/box.h"
#include <wayland-server-core.h>
#include <wayland-util.h>

struct scene {
    struct server_gl *gl;
    struct server_ui *ui;

    uint32_t image_max_size;

    struct {
        struct scene_shader *data;
        size_t count;
    } shaders;

    struct {
        unsigned int debug;
        size_t debug_vtxcount;

        unsigned int font_tex;
    } buffers;

    struct wl_list images;  // scene_image.link
    struct wl_list mirrors; // scene_mirror.link
    struct wl_list text;    // scene_text.link

    int skipped_frames;

    struct wl_listener on_gl_frame;
};

static const int SHADER_SRC_POS_ATTRIB_LOC = 0;
static const int SHADER_DST_POS_ATTRIB_LOC = 1;
static const int SHADER_SRC_RGBA_ATTRIB_LOC = 2;
static const int SHADER_DST_RGBA_ATTRIB_LOC = 3;
struct scene_shader {
    struct server_gl_shader *shader;
    int shader_u_src_size, shader_u_dst_size;

    char *name;
};

struct scene_image_options {
    struct box dst;

    char *shader_name;
};

struct scene_mirror_options {
    struct box src, dst;
    float src_rgba[4];
    float dst_rgba[4];

    char *shader_name;
};

struct scene_text_options {
    int32_t x;
    int32_t y;

    float rgba[4];
    int32_t size_multiplier;

    const char *shader_name;
};

struct scene *scene_create(struct config *cfg, struct server_gl *gl, struct server_ui *ui);
void scene_destroy(struct scene *scene);

struct scene_image *scene_add_image(struct scene *scene, const struct scene_image_options *options,
                                    void *pngbuf, size_t pngbuf_size);
struct scene_mirror *scene_add_mirror(struct scene *scene,
                                      const struct scene_mirror_options *options);
struct scene_text *scene_add_text(struct scene *scene, const char *data,
                                  const struct scene_text_options *options);

void scene_image_destroy(struct scene_image *image);
void scene_mirror_destroy(struct scene_mirror *mirror);
void scene_text_destroy(struct scene_text *text);

#endif
