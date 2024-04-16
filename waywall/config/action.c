#include "config/action.h"
#include "config/api.h"
#include "config/config.h"
#include "config/internal.h"
#include "util/log.h"
#include "util/prelude.h"
#include <luajit-2.1/lauxlib.h>

int
config_action_try(struct config *cfg, struct wall *wall, struct config_action action) {
    char buf[BIND_BUFLEN];
    config_encode_bind(buf, action);

    lua_pushlightuserdata(cfg->L, (void *)&config_registry_keys.actions);
    lua_gettable(cfg->L, LUA_REGISTRYINDEX);

    lua_pushlstring(cfg->L, buf, STATIC_ARRLEN(buf));
    lua_gettable(cfg->L, -2);

    switch (lua_type(cfg->L, -1)) {
    case LUA_TFUNCTION:
        config_api_set_wall(cfg, wall);

        if (config_pcall(cfg, 0, 0, 0) != 0) {
            ww_log(LOG_ERROR, "failed to perform action: '%s'", lua_tostring(cfg->L, -1));
            return -1;
        }

        lua_pop(cfg->L, 1);
        return 1;
    case LUA_TNIL:
        lua_pop(cfg->L, 2);
        return 0;
    default:
        // Non-function values should have been filtered out when the config was loaded.
        ww_unreachable();
    }
}
