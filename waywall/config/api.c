#include "config/api.h"
#include "config/config.h"
#include "config/internal.h"
#include "cpu/cpu.h"
#include "instance.h"
#include "lua/api.h"
#include "lua/helpers.h"
#include "server/server.h"
#include "server/ui.h"
#include "server/wl_seat.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include "wall.h"
#include <luajit-2.1/lauxlib.h>
#include <luajit-2.1/lualib.h>
#include <time.h>
#include <xkbcommon/xkbcommon.h>

static inline uint32_t
timespec_ms(struct timespec *ts) {
    return (uint32_t)(ts->tv_sec * 1000) + (uint32_t)(ts->tv_nsec / 1000000);
}

static struct wall *
get_wall(lua_State *L) {
    lua_pushlightuserdata(L, (void *)&config_registry_keys.wall);
    lua_gettable(L, LUA_REGISTRYINDEX);

    if (!luaL_testudata(L, -1, METATABLE_WALL)) {
        luaL_error(L, "this function cannot be called at startup");
        ww_unreachable();
    }

    struct wall **wall = lua_touserdata(L, -1);
    lua_pop(L, 1);
    return *wall;
}

static int
l_active_instance(lua_State *L) {
    struct wall *wall = get_wall(L);
    int id = wall->active_instance;

    if (id >= 0) {
        lua_pushinteger(L, id + 1);
    } else {
        lua_pushnil(L);
    }

    return 1;
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
l_goto_wall(lua_State *L) {
    struct wall *wall = get_wall(L);

    bool ok = wall_lua_return(wall) == 0;
    if (!ok) {
        return luaL_error(L, "wall already active");
    }

    return 0;
}

static int
l_hovered(lua_State *L) {
    struct wall *wall = get_wall(L);
    int id = wall_lua_get_hovered(wall);

    if (id >= 0) {
        lua_pushinteger(L, id + 1);
    } else {
        lua_pushnil(L);
    }

    return 1;
}

static int
l_instance(lua_State *L) {
    struct wall *wall = get_wall(L);
    int id = luaL_checkint(L, 1);
    luaL_argcheck(L, id >= 1 && id <= wall->num_instances, 1, "invalid instance");

    static const char *screen_names[] = {
        [SCREEN_TITLE] = "title",           [SCREEN_WAITING] = "waiting",
        [SCREEN_GENERATING] = "generating", [SCREEN_PREVIEWING] = "previewing",
        [SCREEN_INWORLD] = "inworld",
    };

    static const char *inworld_names[] = {
        [INWORLD_UNPAUSED] = "unpaused",
        [INWORLD_PAUSED] = "paused",
        [INWORLD_MENU] = "menu",
    };

    struct instance_state state = wall->instances[id - 1]->state;

    lua_newtable(L);

    lua_pushstring(L, "screen");
    lua_pushstring(L, screen_names[state.screen]);
    lua_rawset(L, -3);

    if (state.screen == SCREEN_GENERATING || state.screen == SCREEN_PREVIEWING) {
        lua_pushstring(L, "percent");
        lua_pushinteger(L, state.data.percent);
        lua_rawset(L, -3);
    } else if (state.screen == SCREEN_INWORLD) {
        lua_pushstring(L, "inworld");
        lua_pushstring(L, inworld_names[state.data.inworld]);
        lua_rawset(L, -3);
    }

    lua_pushstring(L, "last_load");
    lua_pushinteger(L, timespec_ms(&state.last_load));
    lua_rawset(L, -3);

    lua_pushstring(L, "last_preview");
    lua_pushinteger(L, timespec_ms(&state.last_preview));
    lua_rawset(L, -3);

    return 1;
}

static int
l_listen(lua_State *L) {
    int argc = lua_gettop(L);

    if (argc != 1) {
        return luaL_error(L, "expected one table argument");
    }
    luaL_argcheck(L, lua_istable(L, 1), 1, "expected table");

    // Copy the functions from the provided table.
    const char *functions[] = {"death",         "install", "preview_percent",
                               "preview_start", "resize",  "spawn"};

    lua_newtable(L);
    for (size_t i = 0; i < STATIC_ARRLEN(functions); i++) {
        lua_pushstring(L, functions[i]);
        lua_rawget(L, -3);

        switch (lua_type(L, -1)) {
        case LUA_TFUNCTION:
            lua_pushstring(L, functions[i]);
            lua_pushvalue(L, -2);
            lua_rawset(L, -4);
            break;
        case LUA_TNIL:
            break;
        default:
            return luaL_error(L, "expected '%s' to be of type 'function', was '%s'", functions[i],
                              luaL_typename(L, -1));
        }

        lua_pop(L, 1);
    }

    // Store the previous set of event handlers.
    lua_pushlightuserdata(L, (void *)&config_registry_keys.events);
    lua_rawget(L, LUA_REGISTRYINDEX);

    // Set the new event handlers.
    lua_pushlightuserdata(L, (void *)&config_registry_keys.events);
    lua_pushvalue(L, -3);
    lua_rawset(L, LUA_REGISTRYINDEX);

    // Call the `install` event handler if the wall global has been set (i.e. this call to `listen`
    // is not a result of the configuration being loaded.)
    lua_pushlightuserdata(L, (void *)&config_registry_keys.wall);
    lua_gettable(L, LUA_REGISTRYINDEX);
    bool has_wall = !lua_isnil(L, -1);
    lua_pop(L, 1);

    if (has_wall) {
        ssize_t stack_start = lua_gettop(L);

        lua_pushstring(L, "install");
        lua_rawget(L, 1);

        switch (lua_type(L, -1)) {
        case LUA_TFUNCTION:
            if (lua_pcall(L, 0, 0, 0) != 0) {
                ww_log(LOG_ERROR, "failed to call 'install' event listener: '%s'",
                       lua_tostring(L, -1));
                lua_pop(L, 1);
            }
            break;
        case LUA_TNIL:
            lua_pop(L, 1);
            break;
        default:
            ww_unreachable();
        }

        ww_assert(lua_gettop(L) == stack_start);
    }

    // There is garbage further down the stack, but the previous table is now at the top.
    // We can return and Lua will take care of it.
    return 1;
}

static int
l_num_instances(lua_State *L) {
    struct wall *wall = get_wall(L);

    lua_pushinteger(L, wall->num_instances);
    return 1;
}

static int
l_play(lua_State *L) {
    struct wall *wall = get_wall(L);
    int id = luaL_checkint(L, 1);
    luaL_argcheck(L, id >= 1 && id <= wall->num_instances, 1, "invalid instance");

    bool ok = wall_lua_play(wall, id - 1) == 0;
    if (!ok) {
        return luaL_error(L, "instance %d already active", id);
    }

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
l_reset(lua_State *L) {
    struct wall *wall = get_wall(L);

    int argc = lua_gettop(L);
    if (argc > 2) {
        return luaL_error(L, "too many arguments: %d > 2", argc);
    } else if (argc == 0) {
        return luaL_error(L, "at least one argument is required");
    }

    bool count = true;
    if (argc == 2) {
        if (lua_type(L, 2) != LUA_TBOOLEAN) {
            return luaL_argerror(L, 2, "expected boolean");
        }
        count = lua_toboolean(L, 2);
    }

    switch (lua_type(L, 1)) {
    case LUA_TNUMBER: {
        int id = luaL_checkinteger(L, 1);
        luaL_argcheck(L, id >= 1 && id <= wall->num_instances, 1, "invalid instance");

        if (wall_lua_reset_one(wall, count, id - 1) == 0) {
            lua_pushinteger(L, 1);
        } else {
            lua_pushinteger(L, 0);
        }

        return 1;
    }
    case LUA_TTABLE: {
        size_t n = lua_objlen(L, 1);
        if (n == 0) {
            lua_pushinteger(L, 0);
            return 1;
        }

        int *ids = zalloc(n, sizeof(int));

        lua_pushnil(L);
        for (size_t i = 0; i < n; i++) {
            ww_assert(lua_next(L, 1) != 0);

            if (!lua_isnumber(L, -1)) {
                free(ids);
                return luaL_error(L, "expected instance ID (number), got %s", luaL_typename(L, -1));
            }
            int id = lua_tointeger(L, -1);
            if (id < 1 || id > wall->num_instances) {
                free(ids);
                return luaL_error(L, "invalid instance: %d", id);
            }

            ids[i] = id - 1;

            lua_pop(L, 1);
        }

        lua_pushinteger(L, wall_lua_reset_many(wall, count, n, ids));
        free(ids);
        return 1;
    }
    default:
        return luaL_argerror(L, 1, "expected number or table");
    }
}

static int
l_set_keymap(lua_State *L) {
    struct wall *wall = get_wall(L);

    int argc = lua_gettop(L);
    if (argc != 1) {
        return luaL_error(L, "expected one table argument");
    }
    luaL_argcheck(L, lua_istable(L, 1), 1, "expected table");

    struct xkb_rule_names rule_names = {0};

    const struct {
        const char *key;
        const char **value;
    } mappings[] = {
        {"layout", &rule_names.layout},   {"model", &rule_names.model},
        {"rules", &rule_names.rules},     {"variant", &rule_names.variant},
        {"options", &rule_names.options},
    };

    char *strings[STATIC_ARRLEN(mappings)] = {0};

    for (size_t i = 0; i < STATIC_ARRLEN(mappings); i++) {
        lua_pushstring(L, mappings[i].key);
        lua_rawget(L, 1);

        switch (lua_type(L, -1)) {
        case LUA_TSTRING: {
            char *value = strdup(lua_tostring(L, -1));
            check_alloc(value);
            strings[i] = value;
            *mappings[i].value = value;
            break;
        }
        case LUA_TNIL:
            break;
        default:
            for (size_t i = 0; i < STATIC_ARRLEN(strings); i++) {
                if (strings[i]) {
                    free(strings[i]);
                }
            }

            return luaL_error(L, "expected '%s' to be of type 'string' or 'nil', was '%s'",
                              mappings[i].key, luaL_typename(L, -1));
        }

        lua_pop(L, 1);
    }

    server_seat_lua_set_keymap(wall->server->seat, &rule_names);

    for (size_t i = 0; i < STATIC_ARRLEN(strings); i++) {
        if (strings[i]) {
            free(strings[i]);
        }
    }

    return 0;
}

static int
l_set_layout(lua_State *L) {
    int argc = lua_gettop(L);

    if (argc != 1) {
        return luaL_error(L, "expected one table argument");
    }
    luaL_argcheck(L, lua_istable(L, 1), 1, "expected table");

    lua_pushlightuserdata(L, (void *)&config_registry_keys.layout);
    lua_pushvalue(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);

    return 0;
}

static int
l_set_priority(lua_State *L) {
    struct wall *wall = get_wall(L);
    int id = luaL_checkint(L, 1);
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    bool priority = lua_toboolean(L, 2);
    luaL_argcheck(L, id >= 1 && id <= wall->num_instances, 1, "invalid instance");

    if (!wall->cpu) {
        return 0;
    }

    cpu_set_priority(wall->cpu, id - 1, priority);
    return 0;
}

static int
l_set_resolution(lua_State *L) {
    struct wall *wall = get_wall(L);
    int id = luaL_checkint(L, 1);
    int32_t width = luaL_checkint(L, 2);
    int32_t height = luaL_checkint(L, 3);
    luaL_argcheck(L, id >= 1 && id <= wall->num_instances, 1, "invalid instance");
    luaL_argcheck(L, width >= 0, 1, "width must be non-negative");
    luaL_argcheck(L, height >= 0, 1, "height must be non-negative");

    bool ok = wall_lua_set_res(wall, id - 1, width, height) == 0;
    if (!ok) {
        return luaL_error(L, "cannot set resolution");
    }

    return 0;
}

static int
l_set_sensitivity(lua_State *L) {
    struct wall *wall = get_wall(L);

    double sens = luaL_checknumber(L, 1);
    luaL_argcheck(L, sens > 0, 1, "sensitivity must be a positive, non-zero number");

    wall->cfg->input.sens = sens;
    return 0;
}

static int
l_window_size(lua_State *L) {
    struct wall *wall = get_wall(L);

    if (!wall->server->ui->mapped) {
        lua_pushinteger(L, 0);
        lua_pushinteger(L, 0);
    } else {
        lua_pushinteger(L, wall->width);
        lua_pushinteger(L, wall->height);
    }

    return 2;
}

static int
l_getenv(lua_State *L) {
    const char *var = luaL_checkstring(L, 1);
    const char *result = getenv(var);
    if (result) {
        lua_pushstring(L, result);
    } else {
        lua_pushnil(L);
    }

    return 1;
}

static int
l_log(lua_State *L) {
    ww_log(LOG_INFO, "lua: %s", lua_tostring(L, 1));
    return 0;
}

static const struct luaL_Reg lua_lib[] = {
    // public (see api.lua)
    {"active_instance", l_active_instance},
    {"current_time", l_current_time},
    {"goto_wall", l_goto_wall},
    {"hovered", l_hovered},
    {"instance", l_instance},
    {"listen", l_listen},
    {"num_instances", l_num_instances},
    {"play", l_play},
    {"profile", l_profile},
    {"reset", l_reset},
    {"set_keymap", l_set_keymap},
    {"set_layout", l_set_layout},
    {"set_priority", l_set_priority},
    {"set_resolution", l_set_resolution},
    {"set_sensitivity", l_set_sensitivity},
    {"window_size", l_window_size},

    // private (see init.lua)
    {"getenv", l_getenv},
    {"log", l_log},
    {NULL, NULL},
};

int
config_api_init(struct config *cfg, const char *profile) {
    lua_getglobal(cfg->L, "_G");
    luaL_register(cfg->L, "priv_waywall", lua_lib);
    lua_pop(cfg->L, 2);

    if (profile) {
        lua_pushlightuserdata(cfg->L, (void *)&config_registry_keys.profile);
        lua_pushstring(cfg->L, profile);
        lua_rawset(cfg->L, LUA_REGISTRYINDEX);
    }

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

    return 0;

fail_pcall_helpers:
fail_loadbuffer_helpers:
fail_pcall:
fail_loadbuffer:
    return 1;
}

void
config_api_set_wall(struct config *cfg, struct wall *wall) {
    ssize_t stack_start = lua_gettop(cfg->L);

    struct wall **udata = lua_newuserdata(cfg->L, sizeof(*udata));
    luaL_getmetatable(cfg->L, METATABLE_WALL);
    lua_setmetatable(cfg->L, -2);
    *udata = wall;

    lua_pushlightuserdata(cfg->L, (void *)&config_registry_keys.wall);
    lua_pushvalue(cfg->L, -2);
    lua_rawset(cfg->L, LUA_REGISTRYINDEX);

    lua_pop(cfg->L, 1);
    ww_assert(lua_gettop(cfg->L) == stack_start);
}
