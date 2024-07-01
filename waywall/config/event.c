#include "config/event.h"
#include "config/api.h"
#include "config/config.h"
#include "config/internal.h"
#include "util/log.h"
#include "util/prelude.h"
#include <luajit-2.1/lua.h>

static void
push_event_callback(struct config *cfg, struct wall *wall, const char *event) {
    static const int IDX_EVENTS = 1;

    lua_pushlightuserdata(cfg->L, (void *)&config_registry_keys.events);
    lua_rawget(cfg->L, LUA_REGISTRYINDEX);
    lua_pushstring(cfg->L, event);
    lua_rawget(cfg->L, IDX_EVENTS);

    // This function intentionally leaves the event callback function (or nil) at the top of the
    // stack.
}

void
config_signal_death(struct config *cfg, struct wall *wall, int id) {
    ww_assert(lua_gettop(cfg->L) == 0);

    push_event_callback(cfg, wall, "death");

    switch (lua_type(cfg->L, -1)) {
    case LUA_TFUNCTION:
        lua_pushinteger(cfg->L, id + 1);
        if (config_pcall(cfg, 1, 0, 0) != 0) {
            ww_log(LOG_ERROR, "failed to call 'death' event listener: '%s'",
                   lua_tostring(cfg->L, -1));
            lua_pop(cfg->L, 1);
        }
        break;
    case LUA_TNIL:
        lua_pop(cfg->L, 1);
        break;
    default:
        ww_unreachable();
    }

    lua_pop(cfg->L, 1);
    ww_assert(lua_gettop(cfg->L) == 0);
}

void
config_signal_preview_percent(struct config *cfg, struct wall *wall, int id, int percent) {
    ww_assert(lua_gettop(cfg->L) == 0);

    push_event_callback(cfg, wall, "prewview_percent");

    switch (lua_type(cfg->L, -1)) {
    case LUA_TFUNCTION:
        lua_pushinteger(cfg->L, id + 1);
        lua_pushinteger(cfg->L, percent);
        if (config_pcall(cfg, 2, 0, 0) != 0) {
            ww_log(LOG_ERROR, "failed to call 'preview_percent' event listener: '%s'",
                   lua_tostring(cfg->L, -1));
            lua_pop(cfg->L, 1);
        }
        break;
    case LUA_TNIL:
        lua_pop(cfg->L, 1);
        break;
    default:
        ww_unreachable();
    }

    lua_pop(cfg->L, 1);
    ww_assert(lua_gettop(cfg->L) == 0);
}

void
config_signal_preview_start(struct config *cfg, struct wall *wall, int id) {
    ww_assert(lua_gettop(cfg->L) == 0);

    push_event_callback(cfg, wall, "preview_start");

    switch (lua_type(cfg->L, -1)) {
    case LUA_TFUNCTION:
        lua_pushinteger(cfg->L, id + 1);
        if (config_pcall(cfg, 1, 0, 0) != 0) {
            ww_log(LOG_ERROR, "failed to call 'preview_start' event listener: '%s'",
                   lua_tostring(cfg->L, -1));
            lua_pop(cfg->L, 1);
        }
        break;
    case LUA_TNIL:
        lua_pop(cfg->L, 1);
        break;
    default:
        ww_unreachable();
    }

    lua_pop(cfg->L, 1);
    ww_assert(lua_gettop(cfg->L) == 0);
}

void
config_signal_resize(struct config *cfg, struct wall *wall, int width, int height) {
    ww_assert(lua_gettop(cfg->L) == 0);

    push_event_callback(cfg, wall, "resize");

    switch (lua_type(cfg->L, -1)) {
    case LUA_TFUNCTION:
        lua_pushinteger(cfg->L, width);
        lua_pushinteger(cfg->L, height);
        if (config_pcall(cfg, 2, 0, 0) != 0) {
            ww_log(LOG_ERROR, "failed to call 'resize' event listener: '%s'",
                   lua_tostring(cfg->L, -1));
            lua_pop(cfg->L, 1);
        }
        break;
    case LUA_TNIL:
        lua_pop(cfg->L, 1);
        break;
    default:
        ww_unreachable();
    }

    lua_pop(cfg->L, 1);
    ww_assert(lua_gettop(cfg->L) == 0);
}

void
config_signal_spawn(struct config *cfg, struct wall *wall, int id) {
    ww_assert(lua_gettop(cfg->L) == 0);

    push_event_callback(cfg, wall, "spawn");

    switch (lua_type(cfg->L, -1)) {
    case LUA_TFUNCTION:
        lua_pushinteger(cfg->L, id + 1);
        if (config_pcall(cfg, 1, 0, 0) != 0) {
            ww_log(LOG_ERROR, "failed to call 'spawn' event listener: '%s'",
                   lua_tostring(cfg->L, -1));
            lua_pop(cfg->L, 1);
        }
        break;
    case LUA_TNIL:
        lua_pop(cfg->L, 1);
        break;
    default:
        ww_unreachable();
    }

    lua_pop(cfg->L, 1);
    ww_assert(lua_gettop(cfg->L) == 0);
}
