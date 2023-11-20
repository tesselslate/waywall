#ifndef WAYWALL_CONFIG_H
#define WAYWALL_CONFIG_H

#include "util.h"
#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#define MAX_ACTIONS 8
#define MAX_BINDS 32
#define IS_INGAME_ACTION(x) ((x) >= ACTION_INGAME_RESET)
#define IS_UNIVERSAL_ACTION(x) ((x) <= ACTION_ANY_TOGGLE_NINB)

extern const char config_filename[];

enum action {
    ACTION_ANY_TOGGLE_NINB,
    ACTION_WALL_RESET_ALL,
    ACTION_WALL_RESET_ONE,
    ACTION_WALL_PLAY,
    ACTION_WALL_PLAY_FIRST_LOCKED,
    ACTION_WALL_LOCK,
    ACTION_WALL_FOCUS_RESET,
    ACTION_INGAME_RESET,
    ACTION_INGAME_ALT_RES,
};

enum unlock_behavior {
    UNLOCK_ACCEPT,
    UNLOCK_IGNORE,
    UNLOCK_RESET,
};

enum ninb_location {
    TOP_LEFT,
    TOP,
    TOP_RIGHT,
    LEFT,
    RIGHT,
    BOTTOM_LEFT,
    BOTTOM_RIGHT,
};

struct bind_input {
    union {
        xkb_keysym_t sym;
        uint32_t button;
    } phys;
    enum {
        BIND_KEY,
        BIND_MOUSE,
    } type;
    uint32_t modifiers;
};

struct keybind {
    struct bind_input input;

    enum action actions[MAX_ACTIONS];
    int action_count;

    bool allow_in_menu;
    bool allow_in_pause;
};

struct config {
    // input
    int repeat_delay;
    int repeat_rate;
    bool confine_pointer;
    double main_sens, alt_sens;

    // keyboard
    char *layout;
    char *rules, *model, *variant, *options;

    // appearance
    float background_color[4];
    char *cursor_theme;
    int cursor_size;
    double ninb_opacity;
    enum ninb_location ninb_location;

    // wall
    int stretch_width, stretch_height;
    int alt_width, alt_height;
    bool use_f1;
    bool remain_in_background;

    // layout
    char *generator_name;
    struct toml_table_t *generator_options;

    // reset options
    enum unlock_behavior unlock_behavior;
    bool count_resets;
    char *resets_file;
    bool wall_bypass;
    int grace_period;

    // performance
    int idle_cpu;
    int low_cpu;
    int high_cpu;
    int active_cpu;
    int preview_threshold;
    char *sleepbg_lock;
    bool force_jit;

    // keybinds
    struct keybind binds[MAX_BINDS];
    int bind_count;

    // not part of config file
    bool has_alt_res;
    bool has_cpu;
    struct toml_table_t *toml;
};

void config_destroy(struct config *config);
char *config_get_dir();
char *config_get_path();
struct config *config_read();

#endif
