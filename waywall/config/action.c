#include "config/action.h"
#include "config/config.h"
#include "config/internal.h"
#include "util/log.h"
#include "util/prelude.h"
#include <inttypes.h>
#include <luajit-2.1/lua.h>
#include <stdbool.h>
#include <sys/types.h>

static bool
start_coro(struct config *cfg, struct config_coro *ccoro) {
    int ret = lua_resume(ccoro->L, 0); // stack: unknown

    switch (ret) {
    case LUA_YIELD:
        // The coroutine has yielded. Let it remain in the coroutines table.
        return true;
    case 0:
        // The coroutine finished. Check if the function returned a value stating that the input
        // should not be consumed.
        if (lua_gettop(ccoro->L) == 0) {
            lua_pushnil(ccoro->L); // stack: unknown (>= 1)
        }
        bool consumed = (!lua_isboolean(ccoro->L, 1) || lua_toboolean(ccoro->L, 1));

        config_coro_delete(ccoro);
        return consumed;
    default:
        // The coroutine failed. Remove it from the coroutines table and log the error.
        ww_log(LOG_ERROR, "failed to start keybind action: '%s'", lua_tostring(ccoro->L, -1));
        config_coro_delete(ccoro);
        return true;
    }
}

bool
config_action_try(struct config *cfg, struct config_action action) {
    static const int IDX_ACTIONS_TABLE = 1;
    static const int IDX_ACTIONS_RESULT = 2;

    ww_assert(lua_gettop(cfg->L) == 0);

    char buf[BIND_BUFLEN];
    config_encode_bind(buf, action);

    lua_pushlightuserdata(cfg->L, (void *)&config_registry_keys.actions); // stack: 1
    lua_gettable(cfg->L, LUA_REGISTRYINDEX); // stack: 1 (IDX_ACTIONS_TABLE)

    lua_pushlstring(cfg->L, buf, STATIC_ARRLEN(buf)); // stack: 2
    lua_gettable(cfg->L, IDX_ACTIONS_TABLE);          // stack: 2 (IDX_ACTIONS_RESULT)

    switch (lua_type(cfg->L, IDX_ACTIONS_RESULT)) {
    case LUA_TFUNCTION: {
        lua_State *coro = lua_newthread(cfg->L); // stack: 3
        struct config_coro *ccoro = config_coro_add(coro);

        // Pop the coroutine itself from the main Lua stack. config_coro_add places it in the global
        // coroutines table so that it will not be garbage collected.
        lua_pop(cfg->L, 1); // stack: 2

        // Cleanup the rest of the main Lua stack now.
        lua_pop(cfg->L, 2); // stack: 0
        ww_assert(lua_gettop(cfg->L) == 0);

        // The new Lua coroutine has a completely separate execution stack, so we need to push the
        // function onto it.
        lua_pushlightuserdata(coro, (void *)&config_registry_keys.actions); // stack: 1
        lua_gettable(coro, LUA_REGISTRYINDEX);                              // stack: 1
        lua_pushlstring(coro, buf, STATIC_ARRLEN(buf));                     // stack: 2
        lua_gettable(coro, IDX_ACTIONS_TABLE);                              // stack: 2

        // Call the function.
        bool consumed = start_coro(cfg, ccoro);
        return consumed;
    }
    case LUA_TNIL:
        lua_pop(cfg->L, 2); // stack: 0
        ww_assert(lua_gettop(cfg->L) == 0);

        return false;
    default:
        // Non-function values should have been filtered out when the config was loaded.
        ww_unreachable();
    }
}
