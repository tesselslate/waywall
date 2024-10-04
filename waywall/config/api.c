#include "config/api.h"
#include "config/config.h"
#include "config/internal.h"
#include "instance.h"
#include "lua/api.h"
#include "lua/helpers.h"
#include "server/server.h"
#include "server/ui.h"
#include "server/wl_seat.h"
#include "server/wp_relative_pointer.h"
#include "timer.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include "wrap.h"
#include <linux/input-event-codes.h>
#include <luajit-2.1/lauxlib.h>
#include <luajit-2.1/lua.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <time.h>
#include <xkbcommon/xkbcommon.h>

#define STARTUP_ERRMSG(function) function " cannot be called during startup"

#define K(x) {#x, KEY_##x}

// TODO: This does not cover all possible keycodes.
static struct {
    const char *name;
    uint8_t code;
} key_mapping[] = {
    K(0),  K(1),  K(2),  K(3),  K(4),  K(5),  K(6),  K(7),  K(8),  K(9),   K(A),   K(B),
    K(C),  K(D),  K(E),  K(F),  K(G),  K(H),  K(I),  K(J),  K(K),  K(L),   K(M),   K(N),
    K(O),  K(P),  K(Q),  K(R),  K(S),  K(T),  K(U),  K(V),  K(W),  K(X),   K(Y),   K(Z),
    K(F1), K(F2), K(F3), K(F4), K(F5), K(F6), K(F7), K(F8), K(F9), K(F10), K(F11), K(F12),
};

#undef K

static void
handle_sleep_alarm(void *data) {
    struct config_coro *ccoro = data;
    if (!ccoro->parent) {
        config_coro_delete(ccoro);
        return;
    }

    lua_settop(ccoro->L, 0);
    int ret = lua_resume(ccoro->L, 0);

    switch (ret) {
    case LUA_YIELD:
        // Do nothing. The coroutine will remain in the table so that it can
        // still be resumed later.
        return;
    case 0:
        // The coroutine finished. Remove it from the coroutines table.
        config_coro_delete(ccoro);
        return;
    default:
        // The coroutine failed. Remove it from the coroutines table and log the error.
        ww_log(LOG_ERROR, "failed to resume keybind action: '%s'", lua_tostring(ccoro->L, -1));
        config_coro_delete(ccoro);
        return;
    }
}

static struct xkb_rule_names
get_rule_names(lua_State *L) {
    struct xkb_rule_names rule_names = {0};

    const struct {
        const char *key;
        const char **value;
    } mappings[] = {
        {"layout", &rule_names.layout},   {"model", &rule_names.model},
        {"rules", &rule_names.rules},     {"variant", &rule_names.variant},
        {"options", &rule_names.options},
    };

    for (size_t i = 0; i < STATIC_ARRLEN(mappings); i++) {
        lua_pushstring(L, mappings[i].key);
        lua_rawget(L, 1);

        switch (lua_type(L, -1)) {
        case LUA_TSTRING: {
            const char *value = lua_tostring(L, -1);
            *mappings[i].value = value;
            break;
        }
        case LUA_TNIL:
            break;
        default:
            luaL_error(L, "expected '%s' to be of type 'string' or 'nil', was '%s'",
                       mappings[i].key, luaL_typename(L, -1));
        }

        lua_pop(L, 1);
    }

    return rule_names;
}

static int
l_current_time(lua_State *L) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint32_t time = (uint32_t)((uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000);

    lua_pushinteger(L, time);

    return 1;
}

static int
l_exec(lua_State *L) {
    struct wrap *wrap = config_get_wrap(L);

    const char *lua_str = luaL_checkstring(L, 1);
    ww_assert(lua_str);

    char *cmd_str = strdup(lua_str);
    check_alloc(cmd_str);

    char *cmd[64] = {0};
    char *needle = cmd_str;
    char *elem;

    bool ok = true;
    size_t i = 0;
    while (ok) {
        elem = needle;
        while (*needle && *needle != ' ') {
            needle++;
        }
        ok = !!*needle;
        *needle = '\0';
        needle++;

        cmd[i++] = elem;
        if (ok && i == STATIC_ARRLEN(cmd)) {
            free(cmd_str);
            return luaL_error(L, "command '%s' contains more than 63 arguments", lua_str);
        }
    }

    wrap_lua_exec(wrap, cmd);
    free(cmd_str);
    return 0;
}

static int
l_active_res(lua_State *L) {
    struct wrap *wrap = config_get_wrap(L);
    if (!wrap) {
        return luaL_error(L, STARTUP_ERRMSG("active_res"));
    }

    lua_pushinteger(L, wrap->active_res.w);
    lua_pushinteger(L, wrap->active_res.h);
    return 2;
}

static int
l_press_key(lua_State *L) {
    struct wrap *wrap = config_get_wrap(L);
    if (!wrap) {
        return luaL_error(L, STARTUP_ERRMSG("press_key"));
    }

    const char *key = luaL_checkstring(L, 1);

    uint32_t keycode = UINT32_MAX;
    for (size_t i = 0; i < STATIC_ARRLEN(key_mapping); i++) {
        if (strcasecmp(key_mapping[i].name, key) == 0) {
            keycode = key_mapping[i].code;
            break;
        }
    }
    if (keycode == UINT32_MAX) {
        return luaL_error(L, "unknown key %s", key);
    }

    wrap_lua_press_key(wrap, keycode);
    return 0;
}

static int
l_profile(lua_State *L) {
    lua_pushlightuserdata(L, (void *)&config_registry_keys.profile);
    lua_rawget(L, LUA_REGISTRYINDEX);

    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
    } else {
        ww_assert(lua_isstring(L, -1));
    }

    return 1;
}

static int
l_set_keymap(lua_State *L) {
    static const int ARG_KEYMAP = 1;

    struct wrap *wrap = config_get_wrap(L);
    if (!wrap) {
        return luaL_error(L, STARTUP_ERRMSG("set_keymap"));
    }

    luaL_argcheck(L, lua_istable(L, ARG_KEYMAP), ARG_KEYMAP, "expected table");

    const struct xkb_rule_names rule_names = get_rule_names(L);
    server_seat_lua_set_keymap(wrap->server->seat, &rule_names);

    return 0;
}

static int
l_set_resolution(lua_State *L) {
    static const int ARG_WIDTH = 1;
    static const int ARG_HEIGHT = 2;

    struct wrap *wrap = config_get_wrap(L);
    if (!wrap) {
        return luaL_error(L, STARTUP_ERRMSG("set_resolution"));
    }

    int32_t width = luaL_checkint(L, ARG_WIDTH);
    int32_t height = luaL_checkint(L, ARG_HEIGHT);

    luaL_argcheck(L, width >= 0, ARG_WIDTH, "width must be non-negative");
    luaL_argcheck(L, height >= 0, ARG_HEIGHT, "height must be non-negative");
    bool ok = wrap_lua_set_res(wrap, width, height) == 0;
    if (!ok) {
        return luaL_error(L, "cannot set resolution");
    }
    return 0;
}

static int
l_set_sensitivity(lua_State *L) {
    static const int ARG_SENS = 1;

    struct wrap *wrap = config_get_wrap(L);
    if (!wrap) {
        return luaL_error(L, STARTUP_ERRMSG("set_sensitivity"));
    }

    double sens = luaL_checknumber(L, ARG_SENS);
    luaL_argcheck(L, sens > 0, ARG_SENS, "sensitivity must be a positive, non-zero number");

    server_relative_pointer_set_sens(wrap->server->relative_pointer, sens);
    return 0;
}

static int
l_show_floating(lua_State *L) {
    static const int ARG_SHOW = 1;

    struct wrap *wrap = config_get_wrap(L);
    if (!wrap) {
        return luaL_error(L, STARTUP_ERRMSG("show_floating"));
    }

    luaL_argcheck(L, lua_type(L, ARG_SHOW) == LUA_TBOOLEAN, ARG_SHOW,
                  "visibility must be a boolean");
    bool show = lua_toboolean(L, ARG_SHOW);

    wrap_lua_show_floating(wrap, show);
    return 0;
}

static int
l_sleep(lua_State *L) {
    static const int ARG_MS = 1;

    struct wrap *wrap = config_get_wrap(L);
    if (!wrap) {
        return luaL_error(L, STARTUP_ERRMSG("sleep"));
    }

    // Ensure that sleep was called from within a coroutine.
    if (lua_pushthread(L) == 1) {
        return luaL_error(L, "sleep called from invalid execution context");
    }

    luaL_argcheck(L, lua_type(L, ARG_MS) == LUA_TNUMBER, ARG_MS, "ms must be a number");
    int ms = lua_tointeger(L, ARG_MS);

    // Setup the timer for this sleep call.
    struct timespec duration = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000,
    };

    struct config_coro *ccoro = config_coro_lookup(L);
    ww_assert(ccoro);

    if (ww_timer_add_entry(wrap->timer, duration, handle_sleep_alarm, ccoro) != 0) {
        return luaL_error(L, "failed to prepare sleep");
    }

    return lua_yield(L, 0);
}

static int
l_state(lua_State *L) {
    struct wrap *wrap = config_get_wrap(L);
    if (!wrap) {
        return luaL_error(L, STARTUP_ERRMSG("state"));
    }
    if (!wrap->instance) {
        return luaL_error(L, "no state output");
    }

    static const char *screen_names[] = {
        [SCREEN_TITLE] = "title",           [SCREEN_WAITING] = "waiting",
        [SCREEN_GENERATING] = "generating", [SCREEN_PREVIEWING] = "previewing",
        [SCREEN_INWORLD] = "inworld",       [SCREEN_WALL] = "wall",
    };

    static const char *inworld_names[] = {
        [INWORLD_UNPAUSED] = "unpaused",
        [INWORLD_PAUSED] = "paused",
        [INWORLD_MENU] = "menu",
    };

    struct instance_state *state = &wrap->instance->state;

    static const int IDX_STATE = 1;

    lua_newtable(L);

    lua_pushstring(L, "screen");
    lua_pushstring(L, screen_names[state->screen]);
    lua_rawset(L, IDX_STATE);

    if (state->screen == SCREEN_GENERATING || state->screen == SCREEN_PREVIEWING) {
        lua_pushstring(L, "percent");
        lua_pushinteger(L, state->data.percent);
        lua_rawset(L, IDX_STATE);
    } else if (state->screen == SCREEN_INWORLD) {
        lua_pushstring(L, "inworld");
        lua_pushstring(L, inworld_names[state->data.inworld]);
        lua_rawset(L, IDX_STATE);
    }

    ww_assert(lua_gettop(L) == IDX_STATE);
    return 1;
}

static int
l_window_size(lua_State *L) {
    struct wrap *wrap = config_get_wrap(L);
    if (!wrap) {
        return luaL_error(L, STARTUP_ERRMSG("window_size"));
    }

    if (!wrap->server->ui->mapped) {
        lua_pushinteger(L, 0);
        lua_pushinteger(L, 0);
    } else {
        lua_pushinteger(L, wrap->width);
        lua_pushinteger(L, wrap->height);
    }
    return 2;
}

static int
l_log(lua_State *L) {
    ww_log(LOG_INFO, "lua: %s", lua_tostring(L, 1));
    return 0;
}

static int
l_log_error(lua_State *L) {
    ww_log(LOG_ERROR, "lua: %s", lua_tostring(L, 1));
    return 0;
}

static int
l_register(lua_State *L) {
    static const int ARG_SIGNAL = 1;
    static const int ARG_HANDLER = 2;

    const char *signal = luaL_checkstring(L, ARG_SIGNAL);
    luaL_argcheck(L, lua_type(L, ARG_HANDLER) == LUA_TFUNCTION, ARG_HANDLER,
                  "handler must be a function");

    static const int IDX_TABLE = 3;

    lua_pushlightuserdata(L, (void *)&config_registry_keys.events);
    lua_rawget(L, LUA_REGISTRYINDEX);

    lua_pushstring(L, signal);
    lua_pushvalue(L, ARG_HANDLER);
    lua_rawset(L, IDX_TABLE);

    return 0;
}

static const struct luaL_Reg lua_lib[] = {
    // public (see api.lua)
    {"active_res", l_active_res},
    {"current_time", l_current_time},
    {"exec", l_exec},
    {"press_key", l_press_key},
    {"profile", l_profile},
    {"set_keymap", l_set_keymap},
    {"set_resolution", l_set_resolution},
    {"set_sensitivity", l_set_sensitivity},
    {"show_floating", l_show_floating},
    {"sleep", l_sleep},
    {"state", l_state},
    {"window_size", l_window_size},

    // private (see init.lua)
    {"log", l_log},
    {"log_error", l_log_error},
    {"register", l_register},
    {NULL, NULL},
};

int
config_api_init(struct config *cfg, const char *profile) {
    ww_assert(lua_gettop(cfg->L) == 0);

    lua_getglobal(cfg->L, "_G");
    luaL_register(cfg->L, "priv_waywall", lua_lib);
    lua_pop(cfg->L, 2);

    if (profile) {
        lua_pushlightuserdata(cfg->L, (void *)&config_registry_keys.profile);
        lua_pushstring(cfg->L, profile);
        lua_rawset(cfg->L, LUA_REGISTRYINDEX);
    }

    lua_pushlightuserdata(cfg->L, (void *)&config_registry_keys.coroutines);
    lua_newtable(cfg->L);
    lua_rawset(cfg->L, LUA_REGISTRYINDEX);

    lua_pushlightuserdata(cfg->L, (void *)&config_registry_keys.events);
    lua_newtable(cfg->L);
    lua_rawset(cfg->L, LUA_REGISTRYINDEX);

    if (luaL_loadbuffer(cfg->L, (const char *)luaJIT_BC_api, luaJIT_BC_api_SIZE, "__api") != 0) {
        ww_log(LOG_ERROR, "failed to load internal api chunk");
        goto fail_loadbuffer;
    }
    if (config_pcall(cfg, 0, 0, 0) != 0) {
        ww_log(LOG_ERROR, "failed to load API: '%s'", lua_tostring(cfg->L, -1));
        goto fail_pcall;
    }

    if (luaL_loadbuffer(cfg->L, (const char *)luaJIT_BC_helpers, luaJIT_BC_helpers_SIZE,
                        "__helpers") != 0) {
        ww_log(LOG_ERROR, "failed to load internal api helpers chunk");
        goto fail_loadbuffer_helpers;
    }
    if (config_pcall(cfg, 0, 0, 0) != 0) {
        ww_log(LOG_ERROR, "failed to load API helpers: '%s'", lua_tostring(cfg->L, -1));
        goto fail_pcall_helpers;
    }

    ww_assert(lua_gettop(cfg->L) == 0);
    return 0;

fail_pcall_helpers:
fail_loadbuffer_helpers:
fail_pcall:
fail_loadbuffer:
    lua_settop(cfg->L, 0);
    return 1;
}

void
config_api_set_wrap(struct config *cfg, struct wrap *wrap) {
    ww_assert(lua_gettop(cfg->L) == 0);

    struct wrap **udata = lua_newuserdata(cfg->L, sizeof(*udata));
    luaL_getmetatable(cfg->L, METATABLE_WRAP);
    lua_setmetatable(cfg->L, -2);
    *udata = wrap;

    lua_pushlightuserdata(cfg->L, (void *)&config_registry_keys.wrap);
    lua_pushvalue(cfg->L, -2);
    lua_rawset(cfg->L, LUA_REGISTRYINDEX);

    lua_pop(cfg->L, 1);
    ww_assert(lua_gettop(cfg->L) == 0);
}

void
config_api_signal(struct config *cfg, const char *signal) {
    static const int IDX_TABLE = 1;

    lua_pushlightuserdata(cfg->L, (void *)&config_registry_keys.events);
    lua_rawget(cfg->L, LUA_REGISTRYINDEX);

    lua_pushstring(cfg->L, signal);
    lua_rawget(cfg->L, IDX_TABLE);

    ww_assert(lua_type(cfg->L, -1) == LUA_TFUNCTION);
    if (config_pcall(cfg, 0, 0, 0) != 0) {
        ww_log(LOG_ERROR, "failed to call event listeners: %s", lua_tostring(cfg->L, -1));
        lua_pop(cfg->L, 1);
    }

    lua_pop(cfg->L, 1);
    ww_assert(lua_gettop(cfg->L) == 0);
}
