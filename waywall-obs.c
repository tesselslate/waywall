/*
 *  Based off of wlrobs by sr.ht/~scoopta
 *
 *  Copyright (C) 2019-2023 Scoopta
 *  This file is part of wlrobs
 *  wlrobs is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  wlrobs is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with wlrobs.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "wlr-export-dmabuf-unstable-v1-protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <obs-module.h>
#include <pthread.h>
#include <unistd.h>
#include <wayland-client.h>

// TODO: (compositor) try to create a new protocol that allows for exporting a DMABUF of the active
//                    instance window for a smoother recording with some refresh rates

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

static struct wl_display *display;
static struct wl_registry *registry;
static struct zwlr_export_dmabuf_manager_v1 *dmabuf_manager;
static struct wl_list outputs;
static bool connected;
static struct timespec retry_at;

static bool in_graphics_ctx;

struct frame {
    struct zwlr_export_dmabuf_frame_v1 *wlr_frame;
    gs_texture_t *tex;
    uint32_t format, width, height, objects;
    uint32_t strides[4], sizes[4], offsets[4], plane_indices[4];
    int32_t fds[4];
    uint64_t modifiers[4];
};

struct output {
    struct wl_output *wl;
    uint32_t name;
    bool is_verification;

    bool frame_queued;
    struct frame *current_frame, *next_frame;
    struct timespec queued_at;
};

struct output_weak {
    struct wl_list link;

    struct output *output;
    int rc;
};

struct waywall_source {
    obs_source_t *src;

    struct output_weak *output_weak;
    bool is_verification;
};

static void
noop() {}

static void
destroy_frame(struct frame *frame) {
    if (frame->objects) {
        for (uint32_t i = 0; i < frame->objects; i++) {
            close(frame->fds[i]);
        }
    }
    if (frame->tex) {
        if (!in_graphics_ctx) {
            obs_enter_graphics();
        }
        gs_texture_destroy(frame->tex);
        if (!in_graphics_ctx) {
            obs_leave_graphics();
        }
    }
    if (frame->wlr_frame) {
        zwlr_export_dmabuf_frame_v1_destroy(frame->wlr_frame);
    }
    free(frame);
}

static void
destroy_output(struct output_weak *weak) {
    assert(weak->output);
    blog(LOG_INFO, "waywall: destroyed output %" PRIu32, weak->output->name);
    wl_output_destroy(weak->output->wl);
    if (weak->output->current_frame) {
        destroy_frame(weak->output->current_frame);
    }
    if (weak->output->next_frame) {
        destroy_frame(weak->output->next_frame);
    }
    free(weak->output);
    weak->output = NULL;
}

static void
deref_output(struct output_weak *weak) {
    assert(weak->rc > 0);
    if (--weak->rc == 0) {
        if (weak->output) {
            destroy_output(weak);
        }
        free(weak);
    }
}

static void
ref_output(struct output_weak *weak) {
    weak->rc++;
}

static void
on_output_name(void *data, struct wl_output *wl_output, const char *name) {
    struct output_weak *weak = data;
    assert(weak->output);
    if (strstr(name, "HEADLESS")) {
        weak->output->is_verification = true;
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
    if (strcmp(interface, zwlr_export_dmabuf_manager_v1_interface.name) == 0) {
        dmabuf_manager =
            wl_registry_bind(registry, name, &zwlr_export_dmabuf_manager_v1_interface, version);
        assert(dmabuf_manager);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (wl_list_length(&outputs) >= 2) {
            blog(LOG_WARNING, "waywall: too many outputs - connected to wrong compositor?");
        }

        struct output *output = calloc(1, sizeof(struct output));
        assert(output);
        output->wl = wl_registry_bind(registry, name, &wl_output_interface, version);
        assert(output->wl);
        output->name = name;
        struct output_weak *weak = calloc(1, sizeof(struct output_weak));
        assert(weak);
        weak->rc = 1;
        weak->output = output;
        wl_list_insert(&outputs, &weak->link);

        wl_output_add_listener(output->wl, &output_listener, weak);
        wl_display_roundtrip(display);
        blog(LOG_INFO, "waywall: found output %" PRIu32 " (headless = %d)", output->name,
             output->is_verification);
    }
}

static void
on_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    struct output_weak *weak, *tmp;

    wl_list_for_each_safe (weak, tmp, &outputs, link) {
        assert(weak->output);
        if (weak->output->name == name) {
            wl_list_remove(&weak->link);
            destroy_output(weak);
            deref_output(weak);
            return;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = on_global,
    .global_remove = on_global_remove,
};

static void
on_dmabuf_cancel(void *data, struct zwlr_export_dmabuf_frame_v1 *wlr_frame, uint32_t reason) {
    struct output *output = data;
    assert(output->next_frame);
    assert(output->next_frame->wlr_frame == wlr_frame);
    // TODO: Is it ever possible to receive a cancel event for an already finished frame?

    output->frame_queued = false;
    destroy_frame(output->next_frame);
    output->next_frame = NULL;

    char *error;
    switch (reason) {
    case ZWLR_EXPORT_DMABUF_FRAME_V1_CANCEL_REASON_TEMPORARY:
        error = "temporary";
        break;
    case ZWLR_EXPORT_DMABUF_FRAME_V1_CANCEL_REASON_PERMANENT:
        error = "permanent";
        break;
    case ZWLR_EXPORT_DMABUF_FRAME_V1_CANCEL_REASON_RESIZING:
        error = "resizing";
        break;
    default:
        error = "unknown";
        break;
    }
    blog(LOG_ERROR, "waywall: dmabuf export cancelled: %s", error);
}

static void
on_dmabuf_frame(void *data, struct zwlr_export_dmabuf_frame_v1 *wlr_frame, uint32_t width,
                uint32_t height, uint32_t offset_x, uint32_t offset_y, uint32_t buffer_flags,
                uint32_t flags, uint32_t format, uint32_t mod_high, uint32_t mod_low,
                uint32_t objects) {
    struct output *output = data;
    assert(output->next_frame);

    output->next_frame->width = width;
    output->next_frame->height = height;
    output->next_frame->format = format;
    output->next_frame->objects = objects;
    for (int i = 0; i < 4; i++) {
        output->next_frame->modifiers[i] = (((uint64_t)mod_high) << 32) | (uint64_t)mod_low;
    }
}

static void
on_dmabuf_object(void *data, struct zwlr_export_dmabuf_frame_v1 *wlr_frame, uint32_t index,
                 int32_t fd, uint32_t size, uint32_t offset, uint32_t stride,
                 uint32_t plane_index) {
    struct output *output = data;
    assert(output->next_frame);

    output->next_frame->fds[index] = fd;
    output->next_frame->sizes[index] = size;
    output->next_frame->offsets[index] = offset;
    output->next_frame->strides[index] = stride;
    output->next_frame->plane_indices[index] = plane_index;
}

static void
on_dmabuf_ready(void *data, struct zwlr_export_dmabuf_frame_v1 *wlr_frame, uint32_t tv_sec_hi,
                uint32_t tv_sec_lo, uint32_t tv_nsec) {
    struct output *output = data;
    assert(output->next_frame);

    if (!in_graphics_ctx) {
        obs_enter_graphics();
    }
    // TODO: Pick appropriate pixel format
    // TODO: Can this fail
    output->next_frame->tex = gs_texture_create_from_dmabuf(
        output->next_frame->width, output->next_frame->height, output->next_frame->format, GS_BGRA,
        output->next_frame->objects, output->next_frame->fds, output->next_frame->strides,
        output->next_frame->offsets, output->next_frame->modifiers);
    assert(output->next_frame->tex);
    if (!in_graphics_ctx) {
        obs_leave_graphics();
    }

    if (output->current_frame) {
        destroy_frame(output->current_frame);
    }
    output->current_frame = output->next_frame;
    output->next_frame = NULL;
    output->frame_queued = false;
}

static const struct zwlr_export_dmabuf_frame_v1_listener dmabuf_listener = {
    .cancel = on_dmabuf_cancel,
    .frame = on_dmabuf_frame,
    .object = on_dmabuf_object,
    .ready = on_dmabuf_ready,
};

static void
disconnect() {
    struct output_weak *weak, *tmp;
    wl_list_for_each_safe (weak, tmp, &outputs, link) {
        wl_list_remove(&weak->link);
        destroy_output(weak);
        deref_output(weak);
    }
    if (dmabuf_manager) {
        zwlr_export_dmabuf_manager_v1_destroy(dmabuf_manager);
        dmabuf_manager = NULL;
    }
    if (registry) {
        wl_registry_destroy(registry);
        registry = NULL;
    }
    if (display) {
        wl_display_disconnect(display);
        display = NULL;
    }
    connected = false;
}

static void
try_connect() {
    int fd = open("/tmp/waywall-display", O_RDONLY);
    if (fd == -1 && errno != ENOENT) {
        blog(LOG_ERROR, "waywall: failed to get waywall display: %s", strerror(errno));
        goto fail;
    }
    char buf[256];
    ssize_t len = read(fd, buf, 255);
    close(fd);
    if (len == -1) {
        blog(LOG_ERROR, "waywall: failed to read waywall display: %s", strerror(errno));
        goto fail;
    }
    buf[len] = '\0';
    char *newline = strchr(buf, '\n');
    if (!newline) {
        blog(LOG_ERROR, "waywall: invalid waywall display");
        goto fail;
    }
    *newline = '\0';

    display = wl_display_connect(buf);
    if (!display) {
        blog(LOG_ERROR, "waywall: failed to connect to waywall display");
        goto fail;
    }

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!dmabuf_manager) {
        blog(LOG_ERROR, "waywall: no dmabuf manager provided by compositor");
        goto fail;
    }

    blog(LOG_INFO, "waywall: connected to waywall display");
    connected = true;
    return;

fail:
    disconnect();
    clock_gettime(CLOCK_MONOTONIC, &retry_at);
    retry_at.tv_sec++;
}

static void
queue_frame(struct output *output) {
    assert(!output->next_frame);
    struct zwlr_export_dmabuf_frame_v1 *wlr_frame =
        zwlr_export_dmabuf_manager_v1_capture_output(dmabuf_manager, true, output->wl);
    assert(wlr_frame);
    output->next_frame = calloc(1, sizeof(struct frame));
    assert(output->next_frame);
    output->next_frame->wlr_frame = wlr_frame;

    output->frame_queued = true;
    zwlr_export_dmabuf_frame_v1_add_listener(wlr_frame, &dmabuf_listener, output);
    clock_gettime(CLOCK_MONOTONIC, &output->queued_at);
    wl_display_roundtrip(display);
}

/*
 *  OBS SOURCE
 */

static void *
waywall_source_create(obs_data_t *settings, obs_source_t *source) {
    struct waywall_source *ww = calloc(1, sizeof(struct waywall_source));
    assert(ww);
    ww->src = source;
    waywall_source_update(ww, settings);

    return ww;
}

static void
waywall_source_destroy(void *data) {
    struct waywall_source *ww = data;
    if (ww->output_weak) {
        deref_output(ww->output_weak);
    }
    free(ww);
}

static void
waywall_source_update(void *data, obs_data_t *settings) {
    struct waywall_source *ww = data;

    bool verif = obs_data_get_bool(settings, "verification");
    if (verif == ww->is_verification) {
        return;
    }

    ww->is_verification = verif;
    if (ww->output_weak) {
        deref_output(ww->output_weak);
        ww->output_weak = NULL;
    }
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
    if (ww->output_weak && ww->output_weak->output) {
        struct frame *frame = ww->output_weak->output->current_frame;
        return frame ? frame->width : 0;
    }
    return 0;
}

static uint32_t
waywall_source_get_height(void *data) {
    struct waywall_source *ww = data;
    if (ww->output_weak && ww->output_weak->output) {
        struct frame *frame = ww->output_weak->output->current_frame;
        return frame ? frame->height : 0;
    }
    return 0;
}

static void
waywall_source_tick(void *data, float seconds) {
    struct waywall_source *ww = data;

    // Try connecting to Wayland.
    if (!connected) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        bool later = now.tv_sec > retry_at.tv_sec ||
                     (now.tv_sec == retry_at.tv_sec && now.tv_nsec >= retry_at.tv_nsec);
        if (!later) {
            return;
        }
        try_connect();
    }

    // Try to find an output if needed.
    if (!ww->output_weak) {
        struct output_weak *weak;
        wl_list_for_each (weak, &outputs, link) {
            assert(weak->output);
            if (weak->output->is_verification == ww->is_verification) {
                ref_output(weak);
                ww->output_weak = weak;
            }
        }
    }
}

static void
waywall_source_render(void *data, gs_effect_t *effect) {
    in_graphics_ctx = true;
    struct waywall_source *ww = data;

    if (!connected) {
        return;
    }

    if (wl_display_roundtrip(display) == -1) {
        blog(LOG_ERROR, "waywall: roundtrip failed");
        disconnect();
        goto end;
    }

    if (!ww->output_weak) {
        goto end;
    }
    if (!ww->output_weak->output) {
        deref_output(ww->output_weak);
        ww->output_weak = NULL;
        goto end;
    }
    struct output *output = ww->output_weak->output;

    if (output->current_frame) {
        gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
        gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
        gs_effect_set_texture(image, output->current_frame->tex);
        while (gs_effect_loop(effect, "Draw")) {
            gs_draw_sprite(output->current_frame->tex, 0, 0, 0);
        }
    }

    if (!output->frame_queued) {
        queue_frame(output);
        wl_display_roundtrip(display);
    }

end:
    in_graphics_ctx = false;
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
    .video_tick = waywall_source_tick,
    .video_render = waywall_source_render,
    .get_properties = waywall_source_properties,
    .icon_type = OBS_ICON_TYPE_GAME_CAPTURE,
};

bool
obs_module_load() {
    obs_register_source(&waywall_source);
    wl_list_init(&outputs);
    try_connect();
    blog(LOG_INFO, "waywall: loaded");
    return true;
}

void
obs_module_unload() {
    disconnect();
    blog(LOG_INFO, "waywall: unloaded");
}
