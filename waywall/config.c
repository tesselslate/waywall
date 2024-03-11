#include "config.h"
#include "util.h"
#include <luajit-2.1/lauxlib.h>
#include <luajit-2.1/lualib.h>
#include <stdlib.h>

static const struct config defaults = {
    .cursor =
        {
            .theme = "default",
            .icon = "left_ptr",
        },
};

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
get_int(struct config *cfg, const char *key, int *dst, const char *full_name, bool required) {
    lua_pushstring(cfg->vm.L, key);
    lua_gettable(cfg->vm.L, -2);

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
    lua_gettable(cfg->vm.L, -2);

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
process_config_cursor(struct config *cfg) {
    if (get_string(cfg, "theme", &cfg->cursor.theme, "cursor.theme", false) != 0) {
        return 1;
    }
    if (get_string(cfg, "icon", &cfg->cursor.icon, "cursor.icon", false) != 0) {
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

    return 0;
}

static int
process_config(struct config *cfg) {
    // waywall.cursor
    lua_pushstring(cfg->vm.L, "cursor");
    lua_gettable(cfg->vm.L, -2);
    switch (lua_type(cfg->vm.L, -1)) {
    case LUA_TTABLE:
        if (process_config_cursor(cfg) != 0) {
            return 1;
        }
        break;
    case LUA_TNIL:
        break;
    default:
        ww_log(LOG_ERROR, "expected 'waywall.cursor' to be of type 'table', was '%s'",
               luaL_typename(cfg->vm.L, -1));
        return 1;
    }
    lua_pop(cfg->vm.L, 1);

    // waywall.wall
    lua_pushstring(cfg->vm.L, "wall");
    lua_gettable(cfg->vm.L, -2);
    switch (lua_type(cfg->vm.L, -1)) {
    case LUA_TTABLE:
        if (process_config_wall(cfg) != 0) {
            return 1;
        }
        break;
    case LUA_TNIL:
        break;
    default:
        ww_log(LOG_ERROR, "expected 'waywall.wall' to be of type 'table', was '%s'",
               luaL_typename(cfg->vm.L, -1));
        return 1;
    }
    lua_pop(cfg->vm.L, 1);

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

    cfg->cursor.theme = strdup(cfg->cursor.theme);
    if (!cfg->cursor.theme) {
        ww_log(LOG_ERROR, "failed to allocate config->cursor.theme");
        goto fail_cursor_theme;
        return NULL;
    }

    cfg->cursor.icon = strdup(cfg->cursor.icon);
    if (!cfg->cursor.icon) {
        ww_log(LOG_ERROR, "failed to allocate config->cursor.icon");
        goto fail_cursor_icon;
        return NULL;
    }

    return cfg;

fail_cursor_icon:
    free(cfg->cursor.theme);

fail_cursor_theme:
    free(cfg);
    return NULL;
}

void
config_destroy(struct config *cfg) {
    free(cfg->cursor.theme);
    free(cfg->cursor.icon);

    if (cfg->vm.L) {
        lua_close(cfg->vm.L);
    }

    free(cfg);
}

char *
config_get_path() {
    struct str pathbuf = {0};

    char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg) {
        if (!str_append(&pathbuf, xdg)) {
            ww_log(LOG_ERROR, "config path too long");
            return NULL;
        }
    } else {
        char *home = getenv("HOME");
        if (!home) {
            ww_log(LOG_ERROR, "no $XDG_CONFIG_HOME or $HOME variables present");
            return NULL;
        }

        if (!str_append(&pathbuf, home)) {
            ww_log(LOG_ERROR, "config path too long");
            return NULL;
        }
        if (!str_append(&pathbuf, "/.config/")) {
            ww_log(LOG_ERROR, "config path too long");
            return NULL;
        }
    }

    if (!str_append(&pathbuf, "/waywall/init.lua")) {
        ww_log(LOG_ERROR, "config path too long");
        return NULL;
    }

    return strdup(pathbuf.data);
}

int
config_populate(struct config *cfg) {
    ww_assert(!cfg->vm.L);

    char *path = config_get_path();
    if (!path) {
        ww_log(LOG_ERROR, "failed to get config path");
        return 1;
    }

    cfg->vm.L = luaL_newstate();
    if (!cfg->vm.L) {
        ww_log(LOG_ERROR, "failed to create lua VM");
        goto fail_lua_newstate;
    }

    lua_newtable(cfg->vm.L);
    lua_setglobal(cfg->vm.L, "waywall");
    if (luaL_dofile(cfg->vm.L, path) != 0) {
        ww_log(LOG_ERROR, "failed to load config: %s", lua_tostring(cfg->vm.L, -1));
        goto fail_dofile;
    }

    lua_getglobal(cfg->vm.L, "waywall");
    int type = lua_type(cfg->vm.L, -1);
    if (type != LUA_TTABLE) {
        ww_log(LOG_ERROR, "expected global value 'waywall' to be of type 'table', got '%s'",
               lua_typename(cfg->vm.L, -1));
        goto fail_global;
    }

    if (!lua_checkstack(cfg->vm.L, 16)) {
        ww_log(LOG_ERROR, "not enough lua stack space");
        goto fail_load;
    }
    if (process_config(cfg) != 0) {
        ww_log(LOG_ERROR, "failed to load config table");
        goto fail_load;
    }

    lua_pop(cfg->vm.L, 1);
    ww_assert(lua_gettop(cfg->vm.L) == 0);

    free(path);
    return 0;

fail_load:
    lua_settop(cfg->vm.L, 0);

fail_global:
fail_dofile:
fail_lua_newstate:
    free(path);
    return 1;
}
