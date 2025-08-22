#include "config/config.h"
#include "config/internal.h"
#include "config/vm.h"
#include "lua/init.h"
#include "server/wl_seat.h"
#include "util/alloc.h"
#include "util/keycodes.h"
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
    .experimental =
        {
            .debug = false,
            .jit = false,
            .tearing = false,
        },
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
            .remaps = {0},
            .repeat_rate = -1,
            .repeat_delay = -1,
            .sens = 1.0,
            .confine = false,
        },
    .theme =
        {
            .background = {0, 0, 0, 255},
            .background_path = "",
            .cursor_theme = "",
            .cursor_icon = "",
            .cursor_size = 0,
            .ninb_anchor = ANCHOR_NONE,
            .ninb_opacity = 1.0,
        },
    .shaders = {0},
};

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
    {"alt", KB_MOD_MOD1},      {"mod1", KB_MOD_MOD1},    {"num", KB_MOD_MOD2},
    {"numlock", KB_MOD_MOD2},  {"mod2", KB_MOD_MOD2},    {"mod3", KB_MOD_MOD3},
    {"super", KB_MOD_MOD4},    {"win", KB_MOD_MOD4},     {"mod4", KB_MOD_MOD4},
    {"mod5", KB_MOD_MOD5},
};

static int
get_bool(struct config *cfg, const char *key, bool *dst, const char *full_name, bool required) {
    lua_pushstring(cfg->vm->L, key); // stack: n+1
    lua_rawget(cfg->vm->L, -2);      // stack: n+1

    switch (lua_type(cfg->vm->L, -1)) {
    case LUA_TBOOLEAN: {
        bool x = lua_toboolean(cfg->vm->L, -1);
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
               luaL_typename(cfg->vm->L, -1));
        return 1;
    }

    lua_pop(cfg->vm->L, 1); // stack: n
    return 0;
}

static int
get_double(struct config *cfg, const char *key, double *dst, const char *full_name, bool required) {
    lua_pushstring(cfg->vm->L, key); // stack: n+1
    lua_rawget(cfg->vm->L, -2);      // stack: n+1

    switch (lua_type(cfg->vm->L, -1)) {
    case LUA_TNUMBER: {
        double x = lua_tonumber(cfg->vm->L, -1);
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
               luaL_typename(cfg->vm->L, -1));
        return 1;
    }

    lua_pop(cfg->vm->L, 1); // stack: n
    return 0;
}

static int
get_int(struct config *cfg, const char *key, int *dst, const char *full_name, bool required) {
    lua_pushstring(cfg->vm->L, key); // stack: n+1
    lua_rawget(cfg->vm->L, -2);      // stack: n+1

    switch (lua_type(cfg->vm->L, -1)) {
    case LUA_TNUMBER: {
        double x = lua_tonumber(cfg->vm->L, -1);
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
               luaL_typename(cfg->vm->L, -1));
        return 1;
    }

    lua_pop(cfg->vm->L, 1); // stack: n
    return 0;
}

/*
 * Puts the string in the Lua config[key] into *dst, freeing the previous value of *dst.
 * Returns 0 iff config[key] is a string (or nil and !required. In this case *dst is unchanged).
 */
static int
get_string(struct config *cfg, const char *key, char **dst, const char *full_name, bool required) {
    lua_pushstring(cfg->vm->L, key); // stack: n+1
    lua_rawget(cfg->vm->L, -2);      // stack: n+1

    switch (lua_type(cfg->vm->L, -1)) {
    case LUA_TSTRING:
        free(*dst);
        *dst = strdup(lua_tostring(cfg->vm->L, -1));
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
               luaL_typename(cfg->vm->L, -1));
        return 1;
    }

    lua_pop(cfg->vm->L, 1); // stack: n
    return 0;
}

static int
get_table(struct config *cfg, const char *key, int (*func)(struct config *), const char *full_name,
          bool required) {
    lua_pushstring(cfg->vm->L, key); // stack: n+1
    lua_rawget(cfg->vm->L, -2);      // stack: n+1

    switch (lua_type(cfg->vm->L, -1)) {
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
               luaL_typename(cfg->vm->L, -1));
        return 1;
    }

    lua_pop(cfg->vm->L, 1); // stack: n
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

        if (strcmp(elem, "*") == 0) {
            if (action->wildcard_modifiers) {
                ww_log(LOG_ERROR, "duplicate wildcard modifier in keybind '%s'", orig);
                goto fail;
            }

            action->wildcard_modifiers = true;
            continue;
        }

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
    for (size_t i = 0; i < STATIC_ARRLEN(util_keycodes); i++) {
        if (strcasecmp(util_keycodes[i].name, input) == 0) {
            *out_data = util_keycodes[i].value;
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

int
config_parse_remap(const char *src, const char *dst, struct config_remap *remap) {
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

void
config_add_remap(struct config_remaps *remaps, struct config_remap remap) {
    void *data = realloc(remaps->data, sizeof(*remaps->data) * (remaps->count + 1));
    check_alloc(data);

    remaps->data = data;
    remaps->data[remaps->count++] = remap;
}

static void
add_shader(struct config *cfg, struct config_shader shader) {
    void *data = realloc(cfg->shaders.data, sizeof(*cfg->shaders.data) * (cfg->shaders.count + 1));
    check_alloc(data);

    cfg->shaders.data = data;
    cfg->shaders.data[cfg->shaders.count++] = shader;
}

static void
add_action(struct config *cfg, struct config_action *action) {
    void *data = realloc(cfg->input.actions.data,
                         sizeof(*cfg->input.actions.data) * (cfg->input.actions.count + 1));
    check_alloc(data);

    action->lua_index = cfg->input.actions.count + 1;

    cfg->input.actions.data = data;
    cfg->input.actions.data[cfg->input.actions.count++] = *action;
}

static int
compare_action(const void *a_void, const void *b_void) {
    const struct config_action *a = a_void;
    const struct config_action *b = b_void;

    return __builtin_popcount(b->modifiers) - __builtin_popcount(a->modifiers);
}

static int
process_config_actions(struct config *cfg) {
    static const int IDX_ACTIONS = 2;
    static const int IDX_DUP_TABLE = 3;
    static const int IDX_ACTION_KEY = 4;
    static const int IDX_ACTION_VAL = 5;

    // stack state
    // 2 (IDX_ACTIONS): config.actions
    // 1              : config
    ww_assert(lua_gettop(cfg->vm->L) == IDX_ACTIONS);

    lua_newtable(cfg->vm->L); // stack: 3 (IDX_DUP_TABLE)
    lua_pushnil(cfg->vm->L);  // stack: 4 (IDX_ACTION_KEY)
    while (lua_next(cfg->vm->L, IDX_ACTIONS)) {
        // stack state
        // 5 (IDX_ACTION_VAL) : config.actions[key] (should be a function)
        // 4 (IDX_ACTION_KEY) : key                 (should be a string)
        // 3 (IDX_DUP_TABLE)  : duplicate actions table
        // 2 (IDX_ACTIONS)    : config.actions
        // 1                  : config
        ww_assert(lua_gettop(cfg->vm->L) == IDX_ACTION_VAL);

        if (!lua_isstring(cfg->vm->L, IDX_ACTION_KEY)) {
            ww_log(LOG_ERROR, "non-string key '%s' found in actions table",
                   lua_tostring(cfg->vm->L, IDX_ACTION_KEY));
            return 1;
        }
        if (!lua_isfunction(cfg->vm->L, IDX_ACTION_VAL)) {
            ww_log(LOG_ERROR, "non-function value for key '%s' found in actions table",
                   lua_tostring(cfg->vm->L, IDX_ACTION_KEY));
            return 1;
        }

        const char *bind = lua_tostring(cfg->vm->L, IDX_ACTION_KEY);
        struct config_action action = {0};
        if (parse_bind(bind, &action) != 0) {
            return 1;
        }

        add_action(cfg, &action);

        // The key (numerical index) and value (action function) need to be pushed to the top of the
        // stack to be put in the duplicate table.
        lua_pushinteger(cfg->vm->L, action.lua_index); // stack: 6 (IDX_ACTION_VAL + 1)
        lua_pushvalue(cfg->vm->L, IDX_ACTION_VAL);     // stack: 7 (IDX_ACTION_VAL + 2)
        lua_rawset(cfg->vm->L, IDX_DUP_TABLE);         // stack: 5 (IDX_ACTION_VAL)

        // Pop the value from the top of the stack. The previous key will be left at the top of the
        // stack for the next call to `lua_next`.
        lua_pop(cfg->vm->L, 1); // stack: 4 (IDX_ACTION_KEY)
        ww_assert(lua_gettop(cfg->vm->L) == IDX_ACTION_KEY);
    }

    if (cfg->input.actions.data) {
        // Sort the action mappings so that those with the most modifier bits set are checked for
        // matching first.
        qsort(cfg->input.actions.data, cfg->input.actions.count, sizeof(*cfg->input.actions.data),
              compare_action);
    }

    // stack state
    // 3 (IDX_DUP_TABLE)  : duplicate actions table
    // 2 (IDX_ACTIONS)    : config.actions
    // 1                  : config
    config_vm_register_actions(cfg->vm, cfg->vm->L);

    // Pop the duplicate actions table which was created at the start of this function.
    lua_pop(cfg->vm->L, 1); // stack: 2 (IDX_ACTIONS)
    ww_assert(lua_gettop(cfg->vm->L) == IDX_ACTIONS);

    return 0;
}

static int
process_config_experimental(struct config *cfg) {
    if (get_bool(cfg, "debug", &cfg->experimental.debug, "experimental.debug", false) != 0) {
        return 1;
    }

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

    // stack state
    // 3 (IDX_REMAPS)     : config.input.remaps
    // 2                  : config.input
    // 1                  : config
    ww_assert(lua_gettop(cfg->vm->L) == IDX_REMAPS);

    lua_pushnil(cfg->vm->L); // stack: 4 (IDX_REMAP_KEY)
    while (lua_next(cfg->vm->L, IDX_REMAPS)) {
        // stack state
        // 5 (IDX_REMAP_VAL) : config.input.remaps[key] (should be a string)
        // 4 (IDX_REMAP_KEY)  : key (should be a string)
        // 3 (IDX_REMAPS)     : config.input.remaps
        // 2                  : config.input
        // 1                  : config

        if (!lua_isstring(cfg->vm->L, IDX_REMAP_KEY)) {
            ww_log(LOG_ERROR, "non-string key '%s' found in remaps table",
                   lua_tostring(cfg->vm->L, IDX_REMAP_KEY));
            return 1;
        }
        if (!lua_isstring(cfg->vm->L, IDX_REMAP_VAL)) {
            ww_log(LOG_ERROR, "non-string value for key '%s' found in remaps table",
                   lua_tostring(cfg->vm->L, IDX_REMAP_KEY));
            return 1;
        }

        const char *src_input = lua_tostring(cfg->vm->L, IDX_REMAP_KEY);
        const char *dst_input = lua_tostring(cfg->vm->L, IDX_REMAP_VAL);

        struct config_remap remap = {0};
        if (config_parse_remap(src_input, dst_input, &remap) != 0) {
            return 1;
        }
        config_add_remap(&cfg->input.remaps, remap);

        // Pop the value from the top of the stack. The previous key will be left at the top of the
        // stack for the next call to `lua_next`.
        lua_pop(cfg->vm->L, 1); // stack: 4 (IDX_REMAP_KEY)
        ww_assert(lua_gettop(cfg->vm->L) == IDX_REMAP_KEY);
    }

    // stack state
    // 3 (IDX_REMAPS)     : config.input.remaps
    // 2                  : config.input
    // 1                  : config
    ww_assert(lua_gettop(cfg->vm->L) == 3);

    return 0;
}

static int
process_config_input(struct config *cfg) {
    // stack state
    // 2:   config.input
    // 1:   config
    ww_assert(lua_gettop(cfg->vm->L) == 2);

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
    // stack state
    // 2:   config.theme
    // 1:   config
    ww_assert(lua_gettop(cfg->vm->L) == 2);

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

    if (get_string(cfg, "background_png", &cfg->theme.background_path, "theme.background_png",
                   false) != 0) {
        return 1;
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

    if (get_double(cfg, "ninb_opacity", &cfg->theme.ninb_opacity, "theme.ninb_opacity", false) !=
        0) {
        return 1;
    }

    return 0;
}

static int
process_config_shaders(struct config *cfg) {
    // stack state
    // 2:   config.shaders
    // 1:   config
    const int IDX_SHADERS = 2;
    const int IDX_SHADER_KEY = 3;

    ww_assert(lua_gettop(cfg->vm->L) == IDX_SHADERS);

    lua_pushnil(cfg->vm->L); // stack: 3 (IDX_SHADER_KEY)
    while (lua_next(cfg->vm->L, IDX_SHADERS)) {
        // stack state
        // 4 (IDX_SHADER_VAL) : config.shaders[key] (should be a table)
        // 3 (IDX_SHADER_KEY) : key (should be a string)
        // 2 (IDX_SHADERS)    : config.shaders
        // 1                  : config

        if (!lua_isstring(cfg->vm->L, IDX_SHADER_KEY)) {
            ww_log(LOG_ERROR, "non-string key '%s' found in shaders table",
                   lua_tostring(cfg->vm->L, IDX_SHADER_KEY));
            return 1;
        }

        char *key = strdup(lua_tostring(cfg->vm->L, IDX_SHADER_KEY));
        char *fragment = NULL, *vertex = NULL;
        if (get_string(cfg, "fragment", &fragment, "shaders[].fragment", false)) {
            free(key);
            return 1;
        }
        if (get_string(cfg, "vertex", &vertex, "shaders[].vertex", false)) {
            free(key);
            return 1;
        }

        struct config_shader shader = {
            .name = key,
            .fragment = fragment,
            .vertex = vertex,
        };
        add_shader(cfg, shader);

        // Pop the value from the top of the stack. The previous key will be left at the top of the
        // stack for the next call to `lua_next`.
        lua_pop(cfg->vm->L, 1); // stack: 3 (IDX_SHADER_KEY)
        ww_assert(lua_gettop(cfg->vm->L) == IDX_SHADER_KEY);
    }

    ww_assert(lua_gettop(cfg->vm->L) == IDX_SHADERS);

    return 0;
}

static int
process_config(struct config *cfg) {
    // stack state
    // 1:   config
    ww_assert(lua_gettop(cfg->vm->L) == 1);

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

    if (get_table(cfg, "shaders", process_config_shaders, "shaders", false) != 0) {
        return 1;
    }

    return 0;
}

static int
load_config(struct config *cfg) {
    static const int ARG_CONFIG = 1;

    ww_assert(lua_gettop(cfg->vm->L) == 0);

    if (luaL_loadbuffer(cfg->vm->L, (const char *)luaJIT_BC_init, luaJIT_BC_init_SIZE,
                        "waywall.init") != 0) {
        ww_log(LOG_ERROR, "failed to load internal init chunk");
        goto fail_loadbuffer;
    }
    if (config_vm_pcall(cfg->vm, 0, 1, 0) != 0) {
        ww_log(LOG_ERROR, "failed to load config: '%s'", lua_tostring(cfg->vm->L, -1));
        goto fail_pcall;
    }

    int type = lua_type(cfg->vm->L, ARG_CONFIG);
    if (type != LUA_TTABLE) {
        ww_log(LOG_ERROR, "expected config value to be of type 'table', got '%s'",
               lua_typename(cfg->vm->L, ARG_CONFIG));
        goto fail_table;
    }

    if (process_config(cfg) != 0) {
        ww_log(LOG_ERROR, "failed to load config table");
        goto fail_load;
    }

    lua_pop(cfg->vm->L, 1); // stack: 0
    ww_assert(lua_gettop(cfg->vm->L) == 0);

    return 0;

fail_load:
fail_table:
fail_pcall:
fail_loadbuffer:
    lua_settop(cfg->vm->L, 0);
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
        {&cfg->theme.background_path, "theme.background_png"},
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

    if (cfg->input.actions.data) {
        free(cfg->input.actions.data);
    }

    free(cfg->input.keymap.layout);
    free(cfg->input.keymap.model);
    free(cfg->input.keymap.rules);
    free(cfg->input.keymap.variant);
    free(cfg->input.keymap.options);
    free(cfg->theme.background_path);
    free(cfg->theme.cursor_theme);
    free(cfg->theme.cursor_icon);

    for (size_t i = 0; i < cfg->shaders.count; i++) {
        free(cfg->shaders.data[i].name);
        free(cfg->shaders.data[i].fragment);
        free(cfg->shaders.data[i].vertex);
    }
    if (cfg->shaders.data) {
        free(cfg->shaders.data);
    }

    if (cfg->vm) {
        config_vm_destroy(cfg->vm);
    }

    free(cfg);
}

ssize_t
config_find_action(struct config *cfg, const struct config_action *action) {
    for (size_t i = 0; i < cfg->input.actions.count; i++) {
        const struct config_action *match = &cfg->input.actions.data[i];

        if (match->type != action->type) {
            continue;
        }
        if (match->data != action->data) {
            continue;
        }

        if (match->wildcard_modifiers) {
            // If there is a modifier wildcard, match->modifiers must be a subset of
            // action->modifiers.
            uint32_t mods = match->modifiers & action->modifiers;
            if (mods != match->modifiers) {
                continue;
            }
        } else {
            // If there is no modifier wildcard, the modifiers should match exactly.
            if (match->modifiers != action->modifiers) {
                continue;
            }
        }

        return match->lua_index;
    }

    return -1;
}

int
config_load(struct config *cfg, const char *profile) {
    ww_assert(!cfg->vm);

    cfg->vm = config_vm_create();
    if (!cfg->vm) {
        return 1;
    }

    if (profile) {
        config_vm_set_profile(cfg->vm, profile);
    }

    if (config_api_init(cfg->vm) != 0) {
        return 1;
    }

    if (load_config(cfg) != 0) {
        return 1;
    }

    if (cfg->experimental.jit) {
        if (!luaJIT_setmode(cfg->vm->L, 0, LUAJIT_MODE_ON)) {
            ww_log(LOG_WARN, "failed to re-enable the JIT");
        } else {
            ww_log(LOG_INFO, "JIT re-enabled");
        }
    }

    ww_assert(lua_gettop(cfg->vm->L) == 0);
    return 0;
}
