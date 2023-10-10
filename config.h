#ifndef __CONFIG_H
#define __CONFIG_H

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

extern const char config_filename[];

enum action {
    ACTION_WALL_RESET_ALL,
    ACTION_WALL_RESET_ONE,
    ACTION_WALL_PLAY,
    ACTION_WALL_LOCK,
    ACTION_WALL_FOCUS_RESET,
    ACTION_INGAME_RESET,
    ACTION_INGAME_ALT_RES,
    ACTION_INGAME_TOGGLE_NINB,
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

struct keybind {
    union {
        xkb_keysym_t sym;
        uint32_t button;
    } input;
    enum {
        BIND_KEY,
        BIND_MOUSE,
    } type;
    uint32_t modifiers;
    enum action actions[MAX_ACTIONS];
    int action_count;
    bool allow_in_menu;
};

struct config {
    // input
    int repeat_delay;
    int repeat_rate;
    bool confine_pointer;
    double main_sens, alt_sens;
    // TODO: alternative languages

    // appearance
    float background_color[4];
    float lock_color[4];
    char *cursor_theme;
    int cursor_size;
    double ninb_opacity;
    enum ninb_location ninb_location;

    // wall
    int wall_width, wall_height;
    int stretch_width, stretch_height;
    int alt_width, alt_height;
    bool use_f1;
    bool remain_in_background;

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

    // keybinds
    struct keybind binds[MAX_BINDS];
    int bind_count;

    // not part of config file
    bool has_alt_res;
    bool has_cpu;
};

static struct {
    const char *name;
    uint8_t code;
} minecraft_keycodes[] = {
    {"0", KEY_0},     {"1", KEY_1},     {"2", KEY_2},     {"3", KEY_3},   {"4", KEY_4},
    {"5", KEY_5},     {"6", KEY_6},     {"7", KEY_7},     {"8", KEY_8},   {"9", KEY_9},
    {"a", KEY_A},     {"b", KEY_B},     {"c", KEY_C},     {"d", KEY_D},   {"e", KEY_E},
    {"f", KEY_F},     {"g", KEY_G},     {"h", KEY_H},     {"i", KEY_I},   {"j", KEY_J},
    {"k", KEY_K},     {"l", KEY_L},     {"m", KEY_M},     {"n", KEY_N},   {"o", KEY_O},
    {"p", KEY_P},     {"q", KEY_Q},     {"r", KEY_R},     {"s", KEY_S},   {"t", KEY_T},
    {"u", KEY_U},     {"v", KEY_V},     {"w", KEY_W},     {"x", KEY_X},   {"y", KEY_Y},
    {"z", KEY_Z},     {"f1", KEY_F1},   {"f2", KEY_F2},   {"f3", KEY_F3}, {"f4", KEY_F4},
    {"f5", KEY_F5},   {"f6", KEY_F6},   {"f7", KEY_F7},   {"f8", KEY_F8}, {"f9", KEY_F9},
    {"f10", KEY_F10}, {"f11", KEY_F11}, {"f12", KEY_F12},
};

static inline uint8_t *
get_minecraft_keycode(const char *name) {
    static const char key_prefix[] = "key.keyboard.";
    if (strlen(name) <= STRING_LEN(key_prefix)) {
        wlr_log(WLR_ERROR, "tried reading minecraft keycode with invalid prefix");
        return NULL;
    }

    for (unsigned long i = 0; i < ARRAY_LEN(minecraft_keycodes); i++) {
        if (strcmp(name + STRING_LEN(key_prefix), minecraft_keycodes[i].name) == 0) {
            return &minecraft_keycodes[i].code;
        }
    }
    return NULL;
}

void config_destroy(struct config *config);
char *config_get_dir();
char *config_get_path();
struct config *config_read();

#endif
