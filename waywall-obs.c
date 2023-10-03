/*
 *  Based off of wlrobs by sr.ht/~scoopta
 *
 *  Copyright (C) 2019-2023 Scoopta
 *  This file is part of wlrobs
 *  wlrobs is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    wlrobs is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with wlrobs.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "wlr-export-dmabuf-unstable-v1-protocol.h"
#include <errno.h>
#include <obs-module.h>
#include <pthread.h>
#include <unistd.h>
#include <wayland-client.h>

#define RETRY_TIMEOUT 100

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("waywall", "en-US")

static void *waywall_source_create(obs_data_t *settings, obs_source_t *source);
static void waywall_source_destroy(void *data);
static void waywall_source_update(void *data, obs_data_t *settings);
static const char *waywall_source_get_name(void *data);
static void waywall_source_get_defaults(obs_data_t *settings);
static uint32_t waywall_source_get_width(void *data);
static uint32_t waywall_source_get_height(void *data);
static void waywall_source_render(void *data, gs_effect_t *effect);

struct frame {
    struct zwlr_export_dmabuf_frame_v1 *wlr;
    gs_texture_t *tex;
    uint32_t format, width, height, obj_count;
    uint32_t strides[4], sizes[4], offsets[4], plane_indicdes[4];
    int32_t fds[4];
    uint64_t modifiers[4];
};

struct output {
    struct wl_list link;

    struct wl_output *wl;
    uint32_t name;

    bool verification;
};

struct waywall_source {
    obs_source_t *src;

    struct wl_display *display;
    struct wl_registry *registry;
    struct zwlr_export_dmabuf_manager_v1 *dmabuf_manager;

    struct wl_list outputs;
    struct output *output;
    struct frame *current, *next;

    bool ready, waiting, capture_verification;
    int retry_timeout;
};

static void
noop() {}

static void
destroy_frame(struct frame *frame) {
    if (frame->obj_count) {
        for (uint32_t i = 0; i < frame->obj_count; i++) {
            close(frame->fds[i]);
        }
    }
    if (frame->tex) {
        gs_texture_destroy(frame->tex);
    }
    if (frame->wlr) {
        zwlr_export_dmabuf_frame_v1_destroy(frame->wlr);
    }
    free(frame);
}

static void
destroy_output(struct output *output) {
    blog(LOG_INFO, "waywall: destroyed output %p", output);
    wl_output_destroy(output->wl);
    wl_list_remove(&output->link);
    free(output);
}

static void
on_output_name(void *data, struct wl_output *wl_output, const char *name) {
    struct output *output = data;
    if (strstr(name, "HEADLESS")) {
        output->verification = true;
    }
}

static const struct wl_output_listener output_listener = {
    .name = on_output_name,

    .description = noop,
    .done = noop,
    .geometry = noop,
    .mode = noop,
    .scale = noop,
};

static void
on_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
          uint32_t version) {
    struct waywall_source *ww = data;

    if (strcmp(interface, zwlr_export_dmabuf_manager_v1_interface.name) == 0) {
        ww->dmabuf_manager =
            wl_registry_bind(registry, name, &zwlr_export_dmabuf_manager_v1_interface, version);
        assert(ww->dmabuf_manager);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (wl_list_length(&ww->outputs) >= 2) {
            blog(LOG_WARNING, "waywall: too many outputs - connected to wrong compositor?");
        }

        struct output *output = calloc(1, sizeof(struct output));
        assert(output);
        output->wl = wl_registry_bind(registry, name, &wl_output_interface, version);
        assert(output->wl);
        wl_output_add_listener(output->wl, &output_listener, output);
        output->name = name;
        wl_list_insert(&ww->outputs, &output->link);

        blog(LOG_INFO, "waywall: found output %p (headless = %d)", output, output->verification);
    }
}

static void
on_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    struct waywall_source *ww = data;
    struct output *output, *tmp;

    wl_list_for_each_safe (output, tmp, &ww->outputs, link) {
        if (output->name == name) {
            if (ww->output == output) {
                ww->output = NULL;
            }
            destroy_output(output);
            return;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = on_global,
    .global_remove = on_global_remove,
};

static void
on_dmabuf_cancel(void *data, struct zwlr_export_dmabuf_frame_v1 *wlr, uint32_t reason) {
    // TODO: Make sure this works properly (don't know how to trigger this tbh)
    struct waywall_source *ww = data;
    assert(ww->current);

    zwlr_export_dmabuf_frame_v1_destroy(wlr);
    struct frame *frame;
    if (ww->current->wlr == wlr) {
        // TODO: shouldn't this always have a texture that needs to be destroyed
        frame = ww->current;
        ww->current = NULL;
    } else if (ww->next && ww->next->wlr == wlr) {
        frame = ww->next;
        ww->next = NULL;
    } else {
        ww->waiting = false;
        return;
    }

    for (uint32_t i = 0; i < frame->obj_count; i++) {
        close(frame->fds[i]);
    }
    ww->waiting = false;
}

static void
on_dmabuf_frame(void *data, struct zwlr_export_dmabuf_frame_v1 *frame, uint32_t width,
                uint32_t height, uint32_t offset_x, uint32_t offset_y, uint32_t buffer_flags,
                uint32_t flags, uint32_t format, uint32_t mod_high, uint32_t mod_low,
                uint32_t num_objects) {
    struct waywall_source *ww = data;
    assert(!ww->next);

    ww->next = calloc(1, sizeof(struct frame));
    ww->next->format = format;
    ww->next->width = width;
    ww->next->height = height;
    ww->next->obj_count = num_objects;
    ww->next->wlr = frame;
    for (int i = 0; i < 4; i++) {
        ww->next->modifiers[i] = (((uint64_t)mod_high) << 32) | (uint64_t)mod_low;
    }
}

static void
on_dmabuf_object(void *data, struct zwlr_export_dmabuf_frame_v1 *frame, uint32_t index, int32_t fd,
                 uint32_t size, uint32_t offset, uint32_t stride, uint32_t plane_index) {
    struct waywall_source *ww = data;
    assert(ww->next);

    ww->next->fds[index] = fd;
    ww->next->sizes[index] = size;
    ww->next->offsets[index] = offset;
    ww->next->strides[index] = stride;
    ww->next->plane_indicdes[index] = plane_index;
}

static void
on_dmabuf_ready(void *data, struct zwlr_export_dmabuf_frame_v1 *frame, uint32_t tv_sec_hi,
                uint32_t tv_sec_lo, uint32_t tv_nsec) {
    struct waywall_source *ww = data;

    ww->next->tex = gs_texture_create_from_dmabuf(
        ww->next->width, ww->next->height, ww->next->format, GS_BGRA, ww->next->obj_count,
        ww->next->fds, ww->next->strides, ww->next->offsets, ww->next->modifiers);
    if (ww->current) {
        if (ww->current->tex) {
            gs_texture_destroy(ww->current->tex);
        }
        if (ww->current->wlr) {
            zwlr_export_dmabuf_frame_v1_destroy(ww->current->wlr);
        }
        for (uint32_t i = 0; i < ww->current->obj_count; i++) {
            close(ww->current->fds[i]);
        }
        free(ww->current);
    }

    ww->current = ww->next;
    ww->next = NULL;
    ww->waiting = false;
}

static const struct zwlr_export_dmabuf_frame_v1_listener dmabuf_listener = {
    .cancel = on_dmabuf_cancel,
    .frame = on_dmabuf_frame,
    .object = on_dmabuf_object,
    .ready = on_dmabuf_ready,
};

static void
handle_disconnect(struct waywall_source *ww) {
    ww->ready = ww->waiting = false;
    ww->retry_timeout = RETRY_TIMEOUT;

    struct output *output, *tmp;
    wl_list_for_each_safe (output, tmp, &ww->outputs, link) {
        destroy_output(output);
    }
    ww->output = NULL;
    if (ww->current) {
        destroy_frame(ww->current);
        ww->current = NULL;
    }
    if (ww->next) {
        destroy_frame(ww->next);
        ww->next = NULL;
    }
    if (ww->dmabuf_manager) {
        zwlr_export_dmabuf_manager_v1_destroy(ww->dmabuf_manager);
        ww->dmabuf_manager = NULL;
    }
    if (ww->registry) {
        wl_registry_destroy(ww->registry);
        ww->registry = NULL;
    }
    if (ww->display) {
        wl_display_disconnect(ww->display);
        ww->display = NULL;
    }
}

static bool
try_connect(struct waywall_source *ww) {
    FILE *file = fopen("/tmp/waywall-display", "r");
    if (!file) {
        blog(LOG_ERROR, "waywall: failed to get waywall display");
        return false;
    }
    char buf[256];
    fgets(buf, 256, file);
    fclose(file);

    ww->display = wl_display_connect(buf);
    if (!ww->display) {
        blog(LOG_ERROR, "waywall: failed to connect to waywall display");
        return false;
    }

    ww->registry = wl_display_get_registry(ww->display);
    wl_registry_add_listener(ww->registry, &registry_listener, ww);
    wl_display_roundtrip(ww->display);
    if (!ww->dmabuf_manager) {
        blog(LOG_ERROR, "waywall: no dmabuf manager provided by compositor");
        wl_display_disconnect(ww->display);
        return false;
    }

    blog(LOG_INFO, "waywall: connected to waywall display");
    ww->ready = true;
    return true;
}

/*
   OBS SOURCE
 */

static void *
waywall_source_create(obs_data_t *settings, obs_source_t *source) {
    struct waywall_source *ww = calloc(1, sizeof(struct waywall_source));
    assert(ww);
    ww->src = source;
    wl_list_init(&ww->outputs);
    if (!try_connect(ww)) {
        ww->retry_timeout = RETRY_TIMEOUT;
    }
    waywall_source_update(ww, settings);

    return ww;
}

static void
waywall_source_destroy(void *data) {
    struct waywall_source *ww = data;
    handle_disconnect(ww);
    free(ww);
}

static void
waywall_source_update(void *data, obs_data_t *settings) {
    struct waywall_source *ww = data;

    bool verif = obs_data_get_bool(settings, "verification");
    if (ww->ready) {
        if (ww->output && ww->capture_verification == verif) {
            return;
        }

        struct output *output;
        wl_list_for_each (output, &ww->outputs, link) {
            if (output->verification == verif) {
                ww->output = output;
                break;
            }
        }
    }
    ww->capture_verification = verif;
}

static const char *
waywall_source_get_name(void *data) {
    return obs_module_text("Waywall");
}

static void
waywall_source_get_defaults(obs_data_t *settings) {
    obs_data_set_default_bool(settings, "verification", false);
}

static uint32_t
waywall_source_get_width(void *data) {
    struct waywall_source *ww = data;
    return ww->current ? ww->current->width : 0;
}

static uint32_t
waywall_source_get_height(void *data) {
    struct waywall_source *ww = data;
    return ww->current ? ww->current->height : 0;
}

static void
waywall_source_render(void *data, gs_effect_t *effect) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    blog(LOG_INFO, "render %" PRIu64, now.tv_sec * 1000 + now.tv_nsec / 1000000);
    struct waywall_source *ww = data;

    if (!ww->ready) {
        if (--ww->retry_timeout < 0) {
            if (try_connect(ww)) {
                goto body;
            }
            ww->retry_timeout = RETRY_TIMEOUT;
        }
        return;
    }
body:
    if (wl_display_roundtrip(ww->display) == -1) {
        blog(LOG_ERROR, "waywall: display died");
        handle_disconnect(ww);
        return;
    }

    blog(LOG_INFO, "render");
    if (!ww->output) {
        blog(LOG_INFO, "!ww->output");
        ww->waiting = false;
        return;
    }

    if (!ww->waiting) {
        blog(LOG_INFO, "!ww->waiting");
        ww->waiting = true;
        struct zwlr_export_dmabuf_frame_v1 *frame =
            zwlr_export_dmabuf_manager_v1_capture_output(ww->dmabuf_manager, true, ww->output->wl);
        assert(frame);
        zwlr_export_dmabuf_frame_v1_add_listener(frame, &dmabuf_listener, ww);
    }
    while (ww->waiting && ww->output) {
        blog(LOG_INFO, "ww->waiting && ww->output (%d, %d)", ww->waiting, ww->output != NULL);
        if (wl_display_roundtrip(ww->display) == -1) {
            blog(LOG_ERROR, "waywall: display died");
            handle_disconnect(ww);
            return;
        }
    }
    if (ww->current) {
        blog(LOG_INFO, "ww->current");
        assert(ww->current->tex);

        gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
        gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
        gs_effect_set_texture(image, ww->current->tex);
        while (gs_effect_loop(effect, "Draw")) {
            gs_draw_sprite(ww->current->tex, 0, 0, 0);
        }
    }
}

static obs_properties_t *
waywall_source_properties(void *data) {
    obs_properties_t *props = obs_properties_create();
    assert(props);

    obs_properties_add_bool(props, "verification", "Verification");
    return props;
}

struct obs_source_info waywall_source = {
    .id = "waywall-capture",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
    .create = waywall_source_create,
    .destroy = waywall_source_destroy,
    .update = waywall_source_update,
    .get_name = waywall_source_get_name,
    .get_defaults = waywall_source_get_defaults,
    .get_width = waywall_source_get_width,
    .get_height = waywall_source_get_height,
    .video_render = waywall_source_render,
    .get_properties = waywall_source_properties,
    .icon_type = OBS_ICON_TYPE_GAME_CAPTURE,
};

bool
obs_module_load() {
    obs_register_source(&waywall_source);
    blog(LOG_INFO, "waywall: loaded");
    return true;
}

void
obs_module_unload() {
    blog(LOG_INFO, "waywall: unloaded");
}
