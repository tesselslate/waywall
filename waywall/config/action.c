#include "config/action.h"
#include "config/config.h"
#include "config/internal.h"
#include "util/log.h"
#include "util/prelude.h"
#include <luajit-2.1/lua.h>
#include <stdbool.h>
#include <sys/types.h>

int
config_action_try(struct config *cfg, struct config_action action) {
    ww_assert(lua_gettop(cfg->L) == 0);

    char buf[BIND_BUFLEN];
    config_encode_bind(buf, action);

    lua_pushlightuserdata(cfg->L, (void *)&config_registry_keys.actions);
    lua_gettable(cfg->L, LUA_REGISTRYINDEX);

    lua_pushlstring(cfg->L, buf, STATIC_ARRLEN(buf));
    lua_gettable(cfg->L, -2);

    switch (lua_type(cfg->L, -1)) {
    case LUA_TFUNCTION:
        if (config_pcall(cfg, 0, 1, 0) != 0) {
            ww_log(LOG_ERROR, "failed to perform action: '%s'", lua_tostring(cfg->L, -1));
            lua_pop(cfg->L, 2);
            return -1;
        }

        bool consumed = (!lua_isboolean(cfg->L, -1) || lua_toboolean(cfg->L, -1));

        lua_pop(cfg->L, 2);
        ww_assert(lua_gettop(cfg->L) == 0);

        return consumed ? 1 : 0;
    case LUA_TNIL:
        lua_pop(cfg->L, 2);
        ww_assert(lua_gettop(cfg->L) == 0);

        return 0;
    default:
        // Non-function values should have been filtered out when the config was loaded.
        ww_unreachable();
    }
}
