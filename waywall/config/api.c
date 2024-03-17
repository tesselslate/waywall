#include "config/api.h"
#include "config/config.h"
#include "config/internal.h"
#include "lua/api.h"
#include "util.h"
#include "wall.h"
#include <luajit-2.1/lauxlib.h>
#include <luajit-2.1/lualib.h>

static struct wall *
get_wall(lua_State *L) {
    lua_pushlightuserdata(L, (void *)&config_registry_keys.wall);
    lua_gettable(L, LUA_REGISTRYINDEX);
    luaL_checkudata(L, -1, METATABLE_WALL);

    struct wall **wall = lua_touserdata(L, -1);
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
l_reset(lua_State *L) {
    struct wall *wall = get_wall(L);

    switch (lua_type(L, 1)) {
    case LUA_TNUMBER: {
        int id = luaL_checkinteger(L, 1);
        luaL_argcheck(L, id >= 1 && id <= wall->num_instances, 1, "invalid instance");

        if (wall_lua_reset_one(wall, id - 1) == 0) {
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

        int *ids = calloc(n, sizeof(int));
        if (!ids) {
            return luaL_error(L, "failed to allocate IDs");
        }

        lua_pushnil(L);
        for (size_t i = 0; i < n; i++) {
            ww_assert(lua_next(L, 1) != 0);

            if (!lua_isnumber(L, -1)) {
                free(ids);
                return luaL_error(L, "expected instance ID (number), got %s", luaL_typename(L, -1));
            }
            int id = lua_tointeger(L, -1);
            if (id < 1 || id > wall->num_instances) {
                return luaL_error(L, "invalid instance: %d", id);
            }

            ids[i] = id - 1;

            lua_pop(L, 1);
        }

        lua_pushinteger(L, wall_lua_reset_many(wall, n, ids));
        free(ids);
        return 1;
    }
    default:
        return luaL_argerror(L, 1, "expected number or table");
    }
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
    {"goto_wall", l_goto_wall},
    {"hovered", l_hovered},
    {"play", l_play},
    {"reset", l_reset},

    // private (see init.lua)
    {"getenv", l_getenv},
    {"log", l_log},
    {NULL, NULL},
};

int
config_api_init(struct config *cfg) {
    lua_getglobal(cfg->L, "_G");
    luaL_register(cfg->L, "priv_waywall", lua_lib);
    lua_pop(cfg->L, 2);

    if (luaL_loadbuffer(cfg->L, (const char *)luaJIT_BC_api, luaJIT_BC_api_SIZE, "__api") != 0) {
        ww_log(LOG_ERROR, "failed to load internal api chunk");
        goto fail_loadbuffer;
    }
    if (lua_pcall(cfg->L, 0, 0, 0) != 0) {
        ww_log(LOG_ERROR, "failed to load API: '%s'", lua_tostring(cfg->L, -1));
        goto fail_pcall;
    }

    return 0;

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
