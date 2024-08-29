#include "config/config.h"
#include "config/action.h"
#include "config/api.h"
#include "config/internal.h"
#include "lua/init.h"
#include "server/wl_seat.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include <linux/input-event-codes.h>
#include <luajit-2.1/lauxlib.h>
#include <luajit-2.1/lua.h>
#include <luajit-2.1/luajit.h>
#include <luajit-2.1/lualib.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

static const struct config defaults = {
    .input =
        {
            .keymap =
                {
                    .layout = "",
                    .model = "",
                    .rules = "",
                    .variant = "",
                    .options = "",
                },
            .repeat_rate = -1,
            .repeat_delay = -1,
            .sens = 1.0,
        },
    .theme =
        {
            .background = {0, 0, 0, 255},
            .cursor_theme = "",
            .cursor_icon = "",
            .cursor_size = 0,
            .ninb_anchor = ANCHOR_NONE,
        },
};

#define K(x) {#x, KEY_##x}

static const struct {
    const char *name;
    uint32_t value;
} keycode_mappings[] = {
    K(ESC),         K(1),          K(2),          K(3),
    K(4),           K(5),          K(6),          K(7),
    K(8),           K(9),          K(0),          K(MINUS),
    K(EQUAL),       K(BACKSPACE),  K(TAB),        K(Q),
    K(W),           K(E),          K(R),          K(T),
    K(Y),           K(U),          K(I),          K(O),
    K(P),           K(LEFTBRACE),  K(RIGHTBRACE), K(ENTER),
    K(LEFTCTRL),    K(A),          K(S),          K(D),
    K(F),           K(G),          K(H),          K(J),
    K(K),           K(L),          K(SEMICOLON),  K(APOSTROPHE),
    K(GRAVE),       K(LEFTSHIFT),  K(BACKSLASH),  K(Z),
    K(X),           K(C),          K(V),          K(B),
    K(N),           K(M),          K(COMMA),      K(DOT),
    K(SLASH),       K(RIGHTSHIFT), K(KPASTERISK), K(LEFTALT),
    K(SPACE),       K(CAPSLOCK),   K(F1),         K(F2),
    K(F3),          K(F4),         K(F5),         K(F6),
    K(F7),          K(F8),         K(F9),         K(F10),
    K(NUMLOCK),     K(SCROLLLOCK), K(KP7),        K(KP8),
    K(KP9),         K(KPMINUS),    K(KP4),        K(KP5),
    K(KP6),         K(KPPLUS),     K(KP1),        K(KP2),
    K(KP3),         K(KP0),        K(KPDOT),      K(ZENKAKUHANKAKU),
    K(102ND),       K(F11),        K(F12),        K(RO),
    K(KATAKANA),    K(HIRAGANA),   K(HENKAN),     K(KATAKANAHIRAGANA),
    K(MUHENKAN),    K(KPJPCOMMA),  K(KPENTER),    K(RIGHTCTRL),
    K(KPSLASH),     K(SYSRQ),      K(RIGHTALT),   K(LINEFEED),
    K(HOME),        K(UP),         K(PAGEUP),     K(LEFT),
    K(RIGHT),       K(END),        K(DOWN),       K(PAGEDOWN),
    K(INSERT),      K(DELETE),     K(MACRO),      K(MUTE),
    K(VOLUMEDOWN),  K(VOLUMEUP),   K(POWER),      K(KPEQUAL),
    K(KPPLUSMINUS), K(PAUSE),      K(SCALE),      K(KPCOMMA),
    K(HANGEUL),     K(HANJA),      K(YEN),        K(LEFTMETA),
    K(RIGHTMETA),   K(F13),        K(F14),        K(F15),
    K(F16),         K(F17),        K(F18),        K(F19),
    K(F20),         K(F21),        K(F22),        K(F23),
    K(F24),
};

#undef K

static const struct {
    const char *name;
    uint32_t value;
} button_mappings[] = {
    {"lmb", BTN_LEFT},   {"m1", BTN_LEFT},      {"mouse1", BTN_LEFT},   {"leftmouse", BTN_LEFT},
    {"rmb", BTN_RIGHT},  {"m2", BTN_RIGHT},     {"mouse2", BTN_RIGHT},  {"rightmouse", BTN_RIGHT},
    {"mmb", BTN_MIDDLE}, {"m3", BTN_MIDDLE},    {"mouse3", BTN_MIDDLE}, {"middlemouse", BTN_MIDDLE},

    {"m4", BTN_SIDE},    {"mb4", BTN_SIDE},     {"mouse4", BTN_SIDE},   {"m5", BTN_EXTRA},
    {"mb5", BTN_EXTRA},  {"mouse5", BTN_EXTRA},
};

static const struct {
    const char *name;
    enum kb_modifier value;
} modifier_mappings[] = {
    {"shift", KB_MOD_SHIFT},   {"caps", KB_MOD_CAPS},    {"lock", KB_MOD_CAPS},
    {"capslock", KB_MOD_CAPS}, {"control", KB_MOD_CTRL}, {"ctrl", KB_MOD_CTRL},
    {"alt", KB_MOD_ALT},       {"mod1", KB_MOD_ALT},     {"mod2", KB_MOD_MOD2},
    {"mod3", KB_MOD_MOD3},     {"super", KB_MOD_LOGO},   {"win", KB_MOD_LOGO},
    {"mod4", KB_MOD_LOGO},     {"mod5", KB_MOD_MOD5},
};

static int
get_bool(struct config *cfg, const char *key, bool *dst, const char *full_name, bool required) {
    lua_pushstring(cfg->L, key);
    lua_rawget(cfg->L, -2);

    switch (lua_type(cfg->L, -1)) {
    case LUA_TBOOLEAN: {
        bool x = lua_toboolean(cfg->L, -1);
        *dst = x;
        break;
    }
    case LUA_TNIL:
        if (required) {
            ww_log(LOG_ERROR, "config property '%s' is required", full_name);
            return 1;
        }
        break;
    default:
        ww_log(LOG_ERROR, "expected '%s' to be of type 'boolean', was '%s'", full_name,
               luaL_typename(cfg->L, -1));
        return 1;
    }

    lua_pop(cfg->L, 1);
    return 0;
}

static int
get_double(struct config *cfg, const char *key, double *dst, const char *full_name, bool required) {
    lua_pushstring(cfg->L, key);
    lua_rawget(cfg->L, -2);

    switch (lua_type(cfg->L, -1)) {
    case LUA_TNUMBER: {
        double x = lua_tonumber(cfg->L, -1);
        *dst = x;
        break;
    }
    case LUA_TNIL:
        if (required) {
            ww_log(LOG_ERROR, "config property '%s' is required", full_name);
            return 1;
        }
        break;
    default:
        ww_log(LOG_ERROR, "expected '%s' to be of type 'number', was '%s'", full_name,
               luaL_typename(cfg->L, -1));
        return 1;
    }

    lua_pop(cfg->L, 1);
    return 0;
}

static int
get_int(struct config *cfg, const char *key, int *dst, const char *full_name, bool required) {
    lua_pushstring(cfg->L, key);
    lua_rawget(cfg->L, -2);

    switch (lua_type(cfg->L, -1)) {
    case LUA_TNUMBER: {
        double x = lua_tonumber(cfg->L, -1);
        int ix = (int)x;
        if (ix != x) {
            ww_log(LOG_ERROR, "expected '%s' to be an integer, got '%lf'", full_name, x);
            return 1;
        }
        *dst = ix;
        break;
    }
    case LUA_TNIL:
        if (required) {
            ww_log(LOG_ERROR, "config property '%s' is required", full_name);
            return 1;
        }
        break;
    default:
        ww_log(LOG_ERROR, "expected '%s' to be of type 'number', was '%s'", full_name,
               luaL_typename(cfg->L, -1));
        return 1;
    }

    lua_pop(cfg->L, 1);
    return 0;
}

static int
get_string(struct config *cfg, const char *key, char **dst, const char *full_name, bool required) {
    lua_pushstring(cfg->L, key);
    lua_rawget(cfg->L, -2);

    switch (lua_type(cfg->L, -1)) {
    case LUA_TSTRING:
        free(*dst);
        *dst = strdup(lua_tostring(cfg->L, -1));
        check_alloc(*dst);
        break;
    case LUA_TNIL:
        if (required) {
            ww_log(LOG_ERROR, "config property '%s' is required", full_name);
            return 1;
        }
        break;
    default:
        ww_log(LOG_ERROR, "expected '%s' to be of type 'string', was '%s'", full_name,
               luaL_typename(cfg->L, -1));
        return 1;
    }

    lua_pop(cfg->L, 1);
    return 0;
}

static int
get_table(struct config *cfg, const char *key, int (*func)(struct config *), const char *full_name,
          bool required) {
    lua_pushstring(cfg->L, key);
    lua_rawget(cfg->L, -2);

    switch (lua_type(cfg->L, -1)) {
    case LUA_TTABLE:
        if (func(cfg) != 0) {
            return 1;
        }
        break;
    case LUA_TNIL:
        if (required) {
            ww_log(LOG_ERROR, "config property '%s' is required", full_name);
            return 1;
        }
        break;
    default:
        ww_log(LOG_ERROR, "expected '%s' to be of type 'table', was '%s'", full_name,
               luaL_typename(cfg->L, -1));
        return 1;
    }

    lua_pop(cfg->L, 1);
    return 0;
}

static int
parse_bind(const char *orig, struct config_action *action) {
    char *bind = strdup(orig);
    check_alloc(bind);

    char *needle = bind;
    char *elem;

    bool ok = true;
    while (ok) {
        elem = needle;

        while (*needle && *needle != '-') {
            needle++;
        }
        ok = !!*needle;
        *needle = '\0';
        needle++;

        xkb_keysym_t sym = xkb_keysym_from_name(elem, XKB_KEYSYM_CASE_INSENSITIVE);
        if (sym != XKB_KEY_NoSymbol) {
            if (action->type == CONFIG_ACTION_BUTTON) {
                ww_log(LOG_ERROR, "keybind '%s' contains both a key and mouse button", orig);
                goto fail;
            }
            action->data = sym;
            action->type = CONFIG_ACTION_KEY;
            continue;
        }

        bool mod_ok = false;
        for (size_t i = 0; i < STATIC_ARRLEN(modifier_mappings); i++) {
            if (strcasecmp(modifier_mappings[i].name, elem) == 0) {
                uint32_t mask = modifier_mappings[i].value;
                if (mask & action->modifiers) {
                    ww_log(LOG_ERROR, "duplicate modifier '%s' in keybind '%s'", elem, orig);
                    goto fail;
                }
                action->modifiers |= mask;
                mod_ok = true;
                break;
            }
        }
        if (mod_ok) {
            continue;
        }

        bool button_ok = false;
        for (size_t i = 0; i < STATIC_ARRLEN(button_mappings); i++) {
            if (strcasecmp(button_mappings[i].name, elem) == 0) {
                if (action->type == CONFIG_ACTION_KEY) {
                    ww_log(LOG_ERROR, "keybind '%s' contains both a key and mouse button", orig);
                    goto fail;
                }
                action->data = button_mappings[i].value;
                action->type = CONFIG_ACTION_BUTTON;
                button_ok = true;
                break;
            }
        }
        if (button_ok) {
            continue;
        }

        ww_log(LOG_ERROR, "unknown component '%s' of keybind '%s'", elem, orig);
        goto fail;
    }

    if (action->type == CONFIG_ACTION_NONE) {
        ww_log(LOG_ERROR, "keybind '%s' has no key or button", orig);
        goto fail;
    }

    free(bind);
    return 0;

fail:
    free(bind);
    return 1;
}

static int
parse_remap_half(const char *input, uint32_t *out_data, enum config_remap_type *out_type) {
    for (size_t i = 0; i < STATIC_ARRLEN(keycode_mappings); i++) {
        if (strcasecmp(keycode_mappings[i].name, input) == 0) {
            *out_data = keycode_mappings[i].value;
            *out_type = CONFIG_REMAP_KEY;
            return 0;
        }
    }

    for (size_t i = 0; i < STATIC_ARRLEN(button_mappings); i++) {
        if (strcasecmp(button_mappings[i].name, input) == 0) {
            *out_data = button_mappings[i].value;
            *out_type = CONFIG_REMAP_BUTTON;
            return 0;
        }
    }

    return 1;
}

static int
parse_remap(const char *src, const char *dst, struct config_remap *remap) {
    if (parse_remap_half(src, &remap->src_data, &remap->src_type) != 0) {
        ww_log(LOG_ERROR, "unknown input '%s' for remapping", src);
        return 1;
    }
    if (parse_remap_half(dst, &remap->dst_data, &remap->dst_type) != 0) {
        ww_log(LOG_ERROR, "unknown output '%s' for remapping", src);
        return 1;
    }

    return 0;
}

static void
add_remap(struct config *cfg, struct config_remap remap) {
    void *data = realloc(cfg->input.remaps.data,
                         sizeof(*cfg->input.remaps.data) * (cfg->input.remaps.count + 1));
    check_alloc(data);

    cfg->input.remaps.data = data;
    cfg->input.remaps.data[cfg->input.remaps.count++] = remap;
}

static int
process_config_actions(struct config *cfg) {
    static const int IDX_ACTIONS = 2;
    static const int IDX_DUP_TABLE = 3;
    static const int IDX_ACTION_KEY = 4;
    static const int IDX_ACTION_VAL = 5;

    // LUA STACK:
    // - config.actions
    // - config
    ww_assert(lua_gettop(cfg->L) == IDX_ACTIONS);

    lua_newtable(cfg->L);

    lua_pushnil(cfg->L);
    while (lua_next(cfg->L, IDX_ACTIONS)) {
        // LUA STACK:
        // - config.actions[key] (expected to be a function)
        // - key (expected to be a string)
        // - duplicate actions table (to be put in registry)
        // - config.actions
        // - config

        if (!lua_isstring(cfg->L, IDX_ACTION_KEY)) {
            ww_log(LOG_ERROR, "non-string key '%s' found in actions table",
                   lua_tostring(cfg->L, IDX_ACTION_KEY));
            return 1;
        }
        if (!lua_isfunction(cfg->L, IDX_ACTION_VAL)) {
            ww_log(LOG_ERROR, "non-function value for key '%s' found in actions table",
                   lua_tostring(cfg->L, IDX_ACTION_KEY));
            return 1;
        }

        const char *bind = lua_tostring(cfg->L, IDX_ACTION_KEY);
        struct config_action action = {0};
        if (parse_bind(bind, &action) != 0) {
            return 1;
        }

        char buf[BIND_BUFLEN];
        config_encode_bind(buf, action);

        // The key (encoded bind) and value (action function) need to be pushed to the top of the
        // stack to be put in the duplicate table.
        lua_pushlstring(cfg->L, buf, STATIC_ARRLEN(buf));
        lua_pushvalue(cfg->L, IDX_ACTION_VAL);
        lua_rawset(cfg->L, IDX_DUP_TABLE);

        // Pop the value from the top of the stack. The previous key will be left at the top of the
        // stack for the next call to `lua_next`.
        lua_pop(cfg->L, 1);
        ww_assert(lua_gettop(cfg->L) == IDX_ACTION_KEY);
    }

    // LUA STACK:
    // - duplicate actions table (to be put in registry)
    // - config.actions
    // - config
    lua_pushlightuserdata(cfg->L, (void *)&config_registry_keys.actions);
    lua_pushvalue(cfg->L, IDX_DUP_TABLE);
    lua_rawset(cfg->L, LUA_REGISTRYINDEX);

    // Pop the duplicate actions table which was created at the start of this function.
    lua_pop(cfg->L, 1);
    ww_assert(lua_gettop(cfg->L) == IDX_ACTIONS);

    return 0;
}

static int
process_config_experimental(struct config *cfg) {
    if (get_bool(cfg, "jit", &cfg->experimental.jit, "experimental.jit", false) != 0) {
        return 1;
    }

    if (get_bool(cfg, "tearing", &cfg->experimental.tearing, "experimental.tearing", false) != 0) {
        return 1;
    }

    return 0;
}

static int
process_config_input_remaps(struct config *cfg) {
    static const int IDX_REMAPS = 3;
    static const int IDX_REMAP_KEY = 4;
    static const int IDX_REMAP_VAL = 5;

    // LUA STACK:
    // - config.input.remaps
    // - config.input
    // - config
    ww_assert(lua_gettop(cfg->L) == IDX_REMAPS);

    lua_pushnil(cfg->L);
    while (lua_next(cfg->L, IDX_REMAPS)) {
        // LUA STACK:
        // - config.input.remaps[key] (expected to be a string)
        // - key (expected to be a string)
        // - config.input.remaps
        // - config.input
        // - config

        if (!lua_isstring(cfg->L, IDX_REMAP_KEY)) {
            ww_log(LOG_ERROR, "non-string key '%s' found in remaps table",
                   lua_tostring(cfg->L, IDX_REMAP_KEY));
            return 1;
        }
        if (!lua_isstring(cfg->L, IDX_REMAP_VAL)) {
            ww_log(LOG_ERROR, "non-string value for key '%s' found in remaps table",
                   lua_tostring(cfg->L, IDX_REMAP_KEY));
            return 1;
        }

        const char *src_input = lua_tostring(cfg->L, IDX_REMAP_KEY);
        const char *dst_input = lua_tostring(cfg->L, IDX_REMAP_VAL);

        struct config_remap remap = {0};
        if (parse_remap(src_input, dst_input, &remap) != 0) {
            return 1;
        }
        add_remap(cfg, remap);

        // Pop the value from the top of the stack. The previous key will be left at the top of the
        // stack for the next call to `lua_next`.
        lua_pop(cfg->L, 1);
        ww_assert(lua_gettop(cfg->L) == IDX_REMAP_KEY);
    }

    // LUA STACK:
    // - config.input.remaps
    // - config.input
    // - config
    ww_assert(lua_gettop(cfg->L) == 3);

    return 0;
}

static int
process_config_input(struct config *cfg) {
    // LUA STACK:
    // - config.input
    // - config
    ww_assert(lua_gettop(cfg->L) == 2);

    if (get_table(cfg, "remaps", process_config_input_remaps, "input.remaps", false) != 0) {
        return 1;
    }

    if (get_string(cfg, "layout", &cfg->input.keymap.layout, "input.layout", false) != 0) {
        return 1;
    }

    if (get_string(cfg, "model", &cfg->input.keymap.model, "input.model", false) != 0) {
        return 1;
    }

    if (get_string(cfg, "rules", &cfg->input.keymap.rules, "input.rules", false) != 0) {
        return 1;
    }

    if (get_string(cfg, "variant", &cfg->input.keymap.variant, "input.variant", false) != 0) {
        return 1;
    }

    if (get_string(cfg, "options", &cfg->input.keymap.options, "input.options", false) != 0) {
        return 1;
    }

    if (get_int(cfg, "repeat_rate", &cfg->input.repeat_rate, "input.repeat_rate", false) != 0) {
        return 1;
    }

    if (get_int(cfg, "repeat_delay", &cfg->input.repeat_delay, "input.repeat_delay", false) != 0) {
        return 1;
    }

    if (get_double(cfg, "sensitivity", &cfg->input.sens, "input.sensitivity", false) != 0) {
        return 1;
    }
    if (cfg->input.sens <= 0) {
        ww_log(LOG_ERROR, "'input.sensitivity' must be a positive, non-zero number");
        return 1;
    }

    if (get_bool(cfg, "confine_pointer", &cfg->input.confine, "input.confine_pointer", false) !=
        0) {
        return 1;
    }

    return 0;
}

static int
process_config_theme(struct config *cfg) {
    // LUA STACK:
    // - config.theme
    // - config
    ww_assert(lua_gettop(cfg->L) == 2);

    char *raw_background = NULL;
    if (get_string(cfg, "background", &raw_background, "theme.background", false) != 0) {
        return 1;
    }
    if (raw_background) {
        if (config_parse_hex(cfg->theme.background, raw_background) != 0) {
            ww_log(LOG_ERROR, "expected 'theme.background' to have a valid hex color, got '%s'",
                   raw_background);
            free(raw_background);
            return 1;
        }
        free(raw_background);
    }

    if (get_string(cfg, "cursor_theme", &cfg->theme.cursor_theme, "theme.cursor_theme", false) !=
        0) {
        return 1;
    }

    if (get_string(cfg, "cursor_icon", &cfg->theme.cursor_icon, "theme.cursor_icon", false) != 0) {
        return 1;
    }

    if (get_int(cfg, "cursor_size", &cfg->theme.cursor_size, "theme.cursor_size", false) != 0) {
        return 1;
    }
    if (cfg->theme.cursor_size < 0) {
        ww_log(LOG_ERROR, "'theme.cursor_size' must be a positive integer");
        return 1;
    }

    char *ninb_anchor = NULL;
    if (get_string(cfg, "ninb_anchor", &ninb_anchor, "theme.ninb_anchor", false) != 0) {
        return 1;
    }
    if (ninb_anchor) {
        static const char *anchor_names[] = {
            [ANCHOR_TOPLEFT] = "topleft",
            [ANCHOR_TOP] = "top",
            [ANCHOR_TOPRIGHT] = "topright",
            [ANCHOR_LEFT] = "left",
            [ANCHOR_RIGHT] = "right",
            [ANCHOR_BOTTOMLEFT] = "bottomleft",
            [ANCHOR_BOTTOMRIGHT] = "bottomright",
        };

        for (size_t i = 0; i < STATIC_ARRLEN(anchor_names); i++) {
            if (strcasecmp(anchor_names[i], ninb_anchor) == 0) {
                cfg->theme.ninb_anchor = i;
                break;
            }
        }

        if (cfg->theme.ninb_anchor == ANCHOR_NONE) {
            ww_log(LOG_ERROR, "invalid value '%s' for 'theme.ninb_anchor'", ninb_anchor);
            free(ninb_anchor);
            return 1;
        }
    }
    free(ninb_anchor);

    return 0;
}

static int
process_config(struct config *cfg) {
    // LUA STACK:
    // - config
    ww_assert(lua_gettop(cfg->L) == 1);

    if (get_table(cfg, "actions", process_config_actions, "actions", true) != 0) {
        return 1;
    }

    if (get_table(cfg, "experimental", process_config_experimental, "experimental", false) != 0) {
        return 1;
    }

    if (get_table(cfg, "input", process_config_input, "input", false) != 0) {
        return 1;
    }

    if (get_table(cfg, "theme", process_config_theme, "theme", false) != 0) {
        return 1;
    }

    return 0;
}

static int
load_config(struct config *cfg) {
    if (luaL_loadbuffer(cfg->L, (const char *)luaJIT_BC_init, luaJIT_BC_init_SIZE, "__init") != 0) {
        ww_log(LOG_ERROR, "failed to load internal init chunk");
        goto fail_loadbuffer;
    }
    if (config_pcall(cfg, 0, 1, 0) != 0) {
        ww_log(LOG_ERROR, "failed to load config: '%s'", lua_tostring(cfg->L, -1));
        goto fail_pcall;
    }

    int type = lua_type(cfg->L, -1);
    if (type != LUA_TTABLE) {
        ww_log(LOG_ERROR, "expected config value to be of type 'table', got '%s'",
               lua_typename(cfg->L, -1));
        goto fail_table;
    }

    if (process_config(cfg) != 0) {
        ww_log(LOG_ERROR, "failed to load config table");
        goto fail_load;
    }

    lua_pop(cfg->L, 1);
    ww_assert(lua_gettop(cfg->L) == 0);

    return 0;

fail_load:
fail_table:
fail_pcall:
fail_loadbuffer:
    lua_settop(cfg->L, 0);
    return 1;
}

struct config *
config_create() {
    struct config *cfg = zalloc(1, sizeof(*cfg));

    // Copy the default configuration, and then heap allocate any strings as needed.
    *cfg = defaults;

    const struct {
        char **storage;
        const char *name;
    } strings[] = {
        {&cfg->input.keymap.layout, "input.layout"},
        {&cfg->input.keymap.model, "input.model"},
        {&cfg->input.keymap.rules, "input.rules"},
        {&cfg->input.keymap.variant, "input.variant"},
        {&cfg->input.keymap.options, "input.options"},
        {&cfg->theme.cursor_theme, "theme.cursor_theme"},
        {&cfg->theme.cursor_icon, "theme.cursor_icon"},
    };

    for (size_t i = 0; i < STATIC_ARRLEN(strings); i++) {
        char **storage = strings[i].storage;

        *storage = strdup(*storage);
        check_alloc(*storage);
    }

    return cfg;
}

void
config_destroy(struct config *cfg) {
    if (cfg->input.remaps.data) {
        free(cfg->input.remaps.data);
    }

    free(cfg->input.keymap.layout);
    free(cfg->input.keymap.model);
    free(cfg->input.keymap.rules);
    free(cfg->input.keymap.variant);
    free(cfg->input.keymap.options);
    free(cfg->theme.cursor_theme);
    free(cfg->theme.cursor_icon);

    if (cfg->L) {
        lua_close(cfg->L);
    }
    free(cfg);
}

int
config_load(struct config *cfg, const char *profile) {
    ww_assert(!cfg->L);

    cfg->L = luaL_newstate();
    if (!cfg->L) {
        ww_log(LOG_ERROR, "failed to create lua VM");
        return 1;
    }

    // The JIT can be re-enabled later if the user enables it in their config.
    if (!luaJIT_setmode(cfg->L, 0, LUAJIT_MODE_OFF)) {
        ww_log(LOG_WARN, "failed to disable the JIT");
    }

    luaL_newmetatable(cfg->L, METATABLE_WALL);
    luaL_newmetatable(cfg->L, METATABLE_WRAP);
    lua_pop(cfg->L, 2);

    luaL_openlibs(cfg->L);

    if (config_api_init(cfg, profile) != 0) {
        goto fail;
    }

    if (load_config(cfg) != 0) {
        goto fail;
    }

    if (cfg->experimental.jit) {
        if (!luaJIT_setmode(cfg->L, 0, LUAJIT_MODE_ON)) {
            ww_log(LOG_WARN, "failed to re-enable the JIT");
        } else {
            ww_log(LOG_INFO, "JIT re-enabled");
        }
    }

    ww_assert(lua_gettop(cfg->L) == 0);
    return 0;

fail:
    lua_close(cfg->L);
    cfg->L = NULL;
    return 1;
}
