#include "config/config.h"
#include "config/action.h"
#include "config/api.h"
#include "config/internal.h"
#include "lua/init.h"
#include "server/wl_seat.h"
#include "util.h"
#include <linux/input-event-codes.h>
#include <luajit-2.1/lauxlib.h>
#include <luajit-2.1/luajit.h>
#include <luajit-2.1/lualib.h>
#include <stdlib.h>
#include <strings.h>
#include <xkbcommon/xkbcommon.h>

static const struct config defaults = {
    .general =
        {
            .counter_path = "",
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
            .repeat_rate = -1,
            .repeat_delay = -1,
            .sens = 1.0,
        },
    .theme =
        {
            .background = {0, 0, 0, 255},
            .cursor_theme = "default",
            .cursor_icon = "left_ptr",
            .cursor_size = 16,
        },
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
        if (!*dst) {
            ww_log(LOG_ERROR, "failed to allocate string for '%s'", full_name);
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
parse_theme_background(struct config *cfg, const char *raw) {
    ssize_t len = strlen(raw);
    bool maybe_valid_rgb = (len == 6) || (len == 7 && raw[0] == '#');
    bool maybe_valid_rgba = (len == 8) || (len == 9 && raw[0] == '#');
    if (!maybe_valid_rgb && !maybe_valid_rgba) {
        goto fail;
    }

    unsigned int r = 0, g = 0, b = 0, a = 255;
    if (maybe_valid_rgb) {
        ssize_t n = sscanf(raw[0] == '#' ? raw + 1 : raw, "%02x%02x%02x", &r, &g, &b);
        if (n != 3) {
            goto fail;
        }
    } else {
        ssize_t n = sscanf(raw[0] == '#' ? raw + 1 : raw, "%02x%02x%02x%02x", &r, &g, &b, &a);
        if (n != 4) {
            goto fail;
        }
    }

    cfg->theme.background[0] = r;
    cfg->theme.background[1] = g;
    cfg->theme.background[2] = b;
    cfg->theme.background[3] = a;

    return 0;

fail:
    ww_log(LOG_ERROR, "expected 'theme.background' to have a valid hex color, got '%s'", raw);
    return 1;
}

static int
parse_bind(const char *orig, struct config_action *action) {
    char *bind = strdup(orig);
    if (!bind) {
        ww_log(LOG_ERROR, "failed to allocate keybind string");
        return 1;
    }

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
process_config_actions(struct config *cfg) {
    ssize_t stack_start = lua_gettop(cfg->L);

    lua_newtable(cfg->L);

    lua_pushnil(cfg->L);
    while (lua_next(cfg->L, -3)) {
        // stack:
        // - value (should be function)
        // - key (should be string)
        // - registry actions table
        // - config.actions
        // - config

        if (!lua_isstring(cfg->L, -2)) {
            ww_log(LOG_ERROR, "non-string key '%s' found in actions table",
                   lua_tostring(cfg->L, -2));
            return 1;
        }
        if (!lua_isfunction(cfg->L, -1)) {
            ww_log(LOG_ERROR, "non-function value for key '%s' found in actions table",
                   lua_tostring(cfg->L, -2));
            return 1;
        }

        const char *bind = lua_tostring(cfg->L, -2);
        struct config_action action = {0};
        if (parse_bind(bind, &action) != 0) {
            return 1;
        }

        char buf[BIND_BUFLEN];
        config_encode_bind(buf, action);

        lua_pushlstring(cfg->L, buf, STATIC_ARRLEN(buf));
        lua_pushvalue(cfg->L, -2);
        lua_rawset(cfg->L, -5);

        // Pop the value from the top of the stack.
        lua_pop(cfg->L, 1);
    }

    // stack:
    // - registry actions table
    // - config.actions
    // - config
    lua_pushlightuserdata(cfg->L, (void *)&config_registry_keys.actions);
    lua_pushvalue(cfg->L, -2);
    lua_rawset(cfg->L, LUA_REGISTRYINDEX);

    // Pop the registry actions table which was created at the start of this function.
    lua_pop(cfg->L, 1);
    ww_assert(lua_gettop(cfg->L) == stack_start);

    return 0;
}

static int
process_config_general(struct config *cfg) {
    if (get_string(cfg, "counter_path", &cfg->general.counter_path, "general.counter_path",
                   false) != 0) {
        return 1;
    }

    return 0;
}

static int
process_config_input(struct config *cfg) {
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
    char *raw_background = NULL;
    if (get_string(cfg, "background", &raw_background, "theme.background", false) != 0) {
        return 1;
    }
    if (raw_background) {
        if (parse_theme_background(cfg, raw_background) != 0) {
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
    if (cfg->theme.cursor_size <= 0) {
        ww_log(LOG_ERROR, "'theme.cursor_size' must be a positive, non-zero integer");
        return 1;
    }

    return 0;
}

static int
process_config_wall(struct config *cfg) {
    if (get_int(cfg, "width", &cfg->wall.width, "wall.width", true) != 0) {
        return 1;
    }
    if (cfg->wall.width <= 0) {
        ww_log(LOG_ERROR, "'wall.width' must be a positive, non-zero integer");
        return 1;
    }

    if (get_int(cfg, "height", &cfg->wall.height, "wall.height", true) != 0) {
        return 1;
    }
    if (cfg->wall.height <= 0) {
        ww_log(LOG_ERROR, "'wall.height' must be a positive, non-zero integer");
        return 1;
    }

    if (get_int(cfg, "stretch_width", &cfg->wall.stretch_width, "wall.stretch_width", true) != 0) {
        return 1;
    }
    if (cfg->wall.stretch_width <= 0) {
        ww_log(LOG_ERROR, "'wall.stretch_width' must be a positive, non-zero integer");
        return 1;
    }

    if (get_int(cfg, "stretch_height", &cfg->wall.stretch_height, "wall.stretch_height", true) !=
        0) {
        return 1;
    }
    if (cfg->wall.stretch_height <= 0) {
        ww_log(LOG_ERROR, "'wall.stretch_height' must be a positive, non-zero integer");
        return 1;
    }

    return 0;
}

static int
process_config(struct config *cfg) {
    if (get_table(cfg, "actions", process_config_actions, "actions", false) != 0) {
        return 1;
    }

    if (get_table(cfg, "general", process_config_general, "general", false) != 0) {
        return 1;
    }

    if (get_table(cfg, "input", process_config_input, "input", false) != 0) {
        return 1;
    }

    if (get_table(cfg, "theme", process_config_theme, "theme", false) != 0) {
        return 1;
    }

    if (get_table(cfg, "wall", process_config_wall, "wall", true) != 0) {
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
    if (lua_pcall(cfg->L, 0, 1, 0) != 0) {
        ww_log(LOG_ERROR, "failed to load config: '%s'", lua_tostring(cfg->L, -1));
        goto fail_pcall;
    }

    int type = lua_type(cfg->L, -1);
    if (type != LUA_TTABLE) {
        ww_log(LOG_ERROR, "expected config value to be of type 'table', got '%s'",
               lua_typename(cfg->L, -1));
        goto fail_table;
    }

    if (!lua_checkstack(cfg->L, 16)) {
        ww_log(LOG_ERROR, "not enough lua stack space");
        goto fail_load;
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
    lua_settop(cfg->L, 0);

fail_pcall:
fail_loadbuffer:
    return 1;
}

struct config *
config_create() {
    struct config *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        ww_log(LOG_ERROR, "failed to allocate config");
        return NULL;
    }

    // Copy the default configuration, and then heap allocate any strings as needed.
    *cfg = defaults;

    const struct {
        char **storage;
        const char *name;
    } strings[] = {
        {&cfg->general.counter_path, "general.counter_path"},
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
        const char *name = strings[i].name;

        *storage = strdup(*storage);
        if (!*storage) {
            ww_log(LOG_ERROR, "failed to allocate %s", name);

            for (size_t j = 0; j < i; j++) {
                free(*strings[j].storage);
            }
            free(cfg);
            return NULL;
        }
    }

    return cfg;
}

void
config_destroy(struct config *cfg) {
    free(cfg->general.counter_path);
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
config_load(struct config *cfg) {
    ww_assert(!cfg->L);

    cfg->L = luaL_newstate();
    if (!cfg->L) {
        ww_log(LOG_ERROR, "failed to create lua VM");
        return 1;
    }

    luaL_newmetatable(cfg->L, METATABLE_WALL);
    lua_pop(cfg->L, 1);

    static const struct luaL_Reg base_lib[] = {
        {"", luaopen_base},         {"package", luaopen_package}, {"table", luaopen_table},
        {"string", luaopen_string}, {"math", luaopen_math},
    };

    for (size_t i = 0; i < STATIC_ARRLEN(base_lib); i++) {
        lua_pushcfunction(cfg->L, base_lib[i].func);
        lua_pushstring(cfg->L, base_lib[i].name);
        lua_call(cfg->L, 1, 0);
    }

    if (config_api_init(cfg) != 0) {
        goto fail;
    }

    if (load_config(cfg) != 0) {
        goto fail;
    }

    return 0;

fail:
    lua_close(cfg->L);
    cfg->L = NULL;
    return 1;
}
