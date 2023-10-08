#ifndef __INSTANCE_H
#define __INSTANCE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

struct state {
    enum {
        TITLE,
        WAITING,
        GENERATING,
        PREVIEWING,
        INWORLD,
    } screen;
    union {
        int percent;
        enum {
            UNPAUSED,
            PAUSED,
            INVENTORY,
        } world;
    } data;
};

struct instance {
    struct window *window;
    const char *dir;
    int state_wd, state_fd;
    struct state state;
    struct timespec last_preview;

    bool alive, locked;
    bool has_stateout, has_wp;
    struct {
        uint8_t atum_hotkey, preview_hotkey;
        int gui_scale;
        bool unicode;
    } options;
    bool alt_res;

    struct wlr_scene_rect *lock_indicator;
    struct headless_view *hview_inst, *hview_wp;
};

bool instance_get_mods(struct instance *);
bool instance_get_options(struct instance *);
bool instance_try_from(struct window *, struct instance *, int);

#endif
