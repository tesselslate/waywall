#include "config.h"
#include "init.lua.h"
#include "util.h"
#include <luajit-2.1/lauxlib.h>
#include <luajit-2.1/lualib.h>
#include <stdlib.h>
#include <xkbcommon/xkbcommon.h>

static const struct config defaults = {
    .input =
        {
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

typedef int (*table_func)(struct config *cfg);

static int
lua_lib_log(lua_State *L) {
    ww_log(LOG_INFO, "lua: %s", lua_tostring(L, 1));
    return 0;
}

static const struct luaL_Reg lua_lib[] = {
    {"log", lua_lib_log},
    {NULL, NULL},
};

static char registry_key = 'w';

// This function is intended for debugging purposes.
// Adapted from: https://stackoverflow.com/a/59097940
__attribute__((unused)) static inline void
dump_stack(struct config *cfg) {
    int n = lua_gettop(cfg->vm.L);
    fprintf(stderr, "--- stack (%d)\n", n);

    for (int i = 1; i <= n; i++) {
        fprintf(stderr, "%d\t%s\t", i, luaL_typename(cfg->vm.L, i));

        switch (lua_type(cfg->vm.L, i)) {
        case LUA_TBOOLEAN:
            fprintf(stderr, lua_toboolean(cfg->vm.L, i) ? "true\n" : "false\n");
            break;
        case LUA_TNUMBER:
            fprintf(stderr, "%lf\n", lua_tonumber(cfg->vm.L, i));
            break;
        case LUA_TSTRING:
            fprintf(stderr, "%s\n", lua_tostring(cfg->vm.L, i));
            break;
        default:
            fprintf(stderr, "%p\n", lua_topointer(cfg->vm.L, i));
            break;
        }
    }
}

static int
get_double(struct config *cfg, const char *key, double *dst, const char *full_name, bool required) {
    lua_pushstring(cfg->vm.L, key);
    lua_rawget(cfg->vm.L, -2);

    switch (lua_type(cfg->vm.L, -1)) {
    case LUA_TNUMBER: {
        double x = lua_tonumber(cfg->vm.L, -1);
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
               luaL_typename(cfg->vm.L, -1));
        return 1;
    }

    lua_pop(cfg->vm.L, 1);
    return 0;
}

static int
get_int(struct config *cfg, const char *key, int *dst, const char *full_name, bool required) {
    lua_pushstring(cfg->vm.L, key);
    lua_rawget(cfg->vm.L, -2);

    switch (lua_type(cfg->vm.L, -1)) {
    case LUA_TNUMBER: {
        double x = lua_tonumber(cfg->vm.L, -1);
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
               luaL_typename(cfg->vm.L, -1));
        return 1;
    }

    lua_pop(cfg->vm.L, 1);
    return 0;
}

static int
get_string(struct config *cfg, const char *key, char **dst, const char *full_name, bool required) {
    lua_pushstring(cfg->vm.L, key);
    lua_rawget(cfg->vm.L, -2);

    switch (lua_type(cfg->vm.L, -1)) {
    case LUA_TSTRING:
        free(*dst);
        *dst = strdup(lua_tostring(cfg->vm.L, -1));
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
               luaL_typename(cfg->vm.L, -1));
        return 1;
    }

    lua_pop(cfg->vm.L, 1);
    return 0;
}

static int
get_table(struct config *cfg, const char *key, table_func func, const char *full_name,
          bool required) {
    lua_pushstring(cfg->vm.L, key);
    lua_rawget(cfg->vm.L, -2);

    switch (lua_type(cfg->vm.L, -1)) {
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
               luaL_typename(cfg->vm.L, -1));
        return 1;
    }

    lua_pop(cfg->vm.L, 1);
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

    int r = 0, g = 0, b = 0, a = 255;
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
get_keymap(struct config *cfg) {
    char *layout = NULL;
    char *model = NULL;
    char *rules = NULL;
    char *variant = NULL;
    char *options = NULL;

    if (get_string(cfg, "layout", &layout, "input.layout", false) != 0) {
        return 1;
    }
    if (get_string(cfg, "model", &model, "input.model", false) != 0) {
        goto fail_model;
    }
    if (get_string(cfg, "rules", &rules, "input.rules", false) != 0) {
        goto fail_rules;
    }
    if (get_string(cfg, "variant", &variant, "input.variant", false) != 0) {
        goto fail_variant;
    }
    if (get_string(cfg, "options", &variant, "input.options", false) != 0) {
        goto fail_options;
    }

    if (layout || model || rules || variant || options) {
        cfg->input.custom_keymap = true;
    }

    struct xkb_rule_names rule_names = {
        .layout = layout,
        .model = model,
        .rules = rules,
        .variant = variant,
        .options = options,
    };

    cfg->input.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!cfg->input.xkb_ctx) {
        goto fail_xkb_ctx;
    }

    // TODO: This leaks memory somehow, even though we later call xkb_keymap_unref
    cfg->input.xkb_keymap =
        xkb_keymap_new_from_names(cfg->input.xkb_ctx, &rule_names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!cfg->input.xkb_keymap) {
        goto fail_xkb_keymap;
    }

    free(options);
    free(variant);
    free(rules);
    free(model);
    free(layout);
    return 0;

fail_xkb_keymap:
    xkb_context_unref(cfg->input.xkb_ctx);
    cfg->input.xkb_ctx = NULL;

fail_xkb_ctx:
    free(options);

fail_options:
    free(variant);

fail_variant:
    free(rules);

fail_rules:
    free(model);

fail_model:
    free(layout);
    return 1;
}

static int
process_config_input(struct config *cfg) {
    if (get_keymap(cfg) != 0) {
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

struct config *
config_create() {
    struct config *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        ww_log(LOG_ERROR, "failed to allocate config");
        return NULL;
    }

    // Copy the default configuration, and then heap allocate any strings as needed.
    *cfg = defaults;

    cfg->theme.cursor_theme = strdup(cfg->theme.cursor_theme);
    if (!cfg->theme.cursor_theme) {
        ww_log(LOG_ERROR, "failed to allocate config->theme.cursor_theme");
        goto fail_cursor_theme;
    }

    cfg->theme.cursor_icon = strdup(cfg->theme.cursor_icon);
    if (!cfg->theme.cursor_icon) {
        ww_log(LOG_ERROR, "failed to allocate config->theme.cursor_icon");
        goto fail_cursor_icon;
    }

    return cfg;

fail_cursor_icon:
    free(cfg->theme.cursor_icon);

fail_cursor_theme:
    free(cfg);
    return NULL;
}

void
config_destroy(struct config *cfg) {
    if (cfg->input.xkb_keymap) {
        xkb_keymap_unref(cfg->input.xkb_keymap);
    }
    if (cfg->input.xkb_ctx) {
        xkb_context_unref(cfg->input.xkb_ctx);
    }

    free(cfg->theme.cursor_theme);
    free(cfg->theme.cursor_icon);

    if (cfg->vm.L) {
        lua_close(cfg->vm.L);
    }

    free(cfg);
}

int
config_populate(struct config *cfg) {
    ww_assert(!cfg->vm.L);

    cfg->vm.L = luaL_newstate();
    if (!cfg->vm.L) {
        ww_log(LOG_ERROR, "failed to create lua VM");
        goto fail_lua_newstate;
    }

    luaL_openlibs(cfg->vm.L);
    lua_getglobal(cfg->vm.L, "_G");
    luaL_register(cfg->vm.L, "priv_waywall", lua_lib);
    lua_pop(cfg->vm.L, 2);

    if (luaL_loadbuffer(cfg->vm.L, (const char *)luaJIT_BC_init, luaJIT_BC_init_SIZE, "__init") !=
        0) {
        ww_log(LOG_ERROR, "failed to load internal init chunk");
        goto fail_loadbuffer;
    }
    if (lua_pcall(cfg->vm.L, 0, 1, 0) != 0) {
        ww_log(LOG_ERROR, "failed to load config: '%s'", lua_tostring(cfg->vm.L, -1));
        goto fail_pcall;
    }

    int type = lua_type(cfg->vm.L, -1);
    if (type != LUA_TTABLE) {
        ww_log(LOG_ERROR, "expected config value to be of type 'table', got '%s'",
               lua_typename(cfg->vm.L, -1));
        goto fail_table;
    }

    if (!lua_checkstack(cfg->vm.L, 16)) {
        ww_log(LOG_ERROR, "not enough lua stack space");
        goto fail_load;
    }
    if (process_config(cfg) != 0) {
        ww_log(LOG_ERROR, "failed to load config table");
        goto fail_load;
    }

    // Store the config table in the Lua registry for later access.
    lua_pushlightuserdata(cfg->vm.L, &registry_key);
    lua_pushvalue(cfg->vm.L, -2);
    lua_settable(cfg->vm.L, LUA_REGISTRYINDEX);

    lua_pop(cfg->vm.L, 1);
    ww_assert(lua_gettop(cfg->vm.L) == 0);

    return 0;

fail_load:
    lua_settop(cfg->vm.L, 0);

fail_table:
fail_pcall:
fail_loadbuffer:
fail_lua_newstate:
    return 1;
}
