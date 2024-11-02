#ifndef WAYWALL_SCENE_H
#define WAYWALL_SCENE_H

#include "util/box.h"
#include <wayland-server-core.h>
#include <wayland-util.h>

struct scene {
    struct server_gl *gl;
    struct server_ui *ui;

    uint32_t image_max_size;

    struct {
        struct server_gl_shader *texcopy;
        int texcopy_u_src_size, texcopy_u_dst_size;
        int texcopy_a_src_pos, texcopy_a_dst_pos, texcopy_a_src_rgba, texcopy_a_dst_rgba;
    } shaders;

    struct {
        unsigned int mirrors;
        size_t mirrors_vtxcount;

        unsigned int font_tex;
    } buffers;

    struct wl_list images;  // scene_image.link
    struct wl_list mirrors; // scene_mirror.link
    struct wl_list text;    // scene_text.link

    struct wl_listener on_gl_frame;
};

struct scene_image_options {
    struct box dst;
};

struct scene_mirror_options {
    struct box src, dst;
    float src_rgba[4];
    float dst_rgba[4];
};

struct scene *scene_create(struct server_gl *gl, struct server_ui *ui);
void scene_destroy(struct scene *scene);

struct scene_image *scene_add_image(struct scene *scene, const struct scene_image_options *options,
                                    void *pngbuf, size_t pngbuf_size);
struct scene_mirror *scene_add_mirror(struct scene *scene,
                                      const struct scene_mirror_options *options);
struct scene_text *scene_add_text(struct scene *scene, const char *data, int32_t x, int32_t y);

void scene_image_destroy(struct scene_image *image);
void scene_mirror_destroy(struct scene_mirror *mirror);
void scene_text_destroy(struct scene_text *text);

#endif
