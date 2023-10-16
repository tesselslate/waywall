#ifndef WAYWALL_INSTANCE_H
#define WAYWALL_INSTANCE_H

#include "compositor.h"
#include "cpu.h"
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
            MENU,
        } world;
    } data;
};

struct instance {
    struct window *window;
    char *dir;
    int state_wd, state_fd;
    struct state state;
    struct timespec last_preview;
    enum cpu_group last_group;

    bool alive, locked;
    bool has_stateout, has_wp;
    struct {
        uint8_t atum_hotkey, preview_hotkey;
        int gui_scale;
        bool unicode;
    } options;
    bool alt_res;

    render_rect_t *lock_indicator;
    struct hview *hview_inst, *hview_wp;
};

bool instance_get_mods(struct instance *instance);
bool instance_get_options(struct instance *instance);
bool instance_try_from(struct window *window, struct instance *instance, int inotify_fd);

#endif