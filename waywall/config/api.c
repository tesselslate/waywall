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
#include "server/wp_relative_pointer.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include "wall.h"
#include "wrap.h"
#include <luajit-2.1/lauxlib.h>
#include <luajit-2.1/lua.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <xkbcommon/xkbcommon.h>

#define STARTUP_ERRMSG(function) function " cannot be called during startup"
#define WRAP_ERRMSG(function) function " cannot be called in wrap mode"

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

static inline uint32_t
timespec_ms(struct timespec *ts) {
    return (uint32_t)(ts->tv_sec * 1000) + (uint32_t)(ts->tv_nsec / 1000000);
}

static struct wall *
get_wall(lua_State *L) {
    ssize_t stack_start = lua_gettop(L);

    lua_pushlightuserdata(L, (void *)&config_registry_keys.wall);
    lua_rawget(L, LUA_REGISTRYINDEX);

    if (!luaL_testudata(L, -1, METATABLE_WALL)) {
        lua_pop(L, 1);
        ww_assert(lua_gettop(L) == stack_start);

        return NULL;
    }

    struct wall **wall = lua_touserdata(L, -1);

    lua_pop(L, 1);
    ww_assert(lua_gettop(L) == stack_start);

    return *wall;
}

static struct wrap *
get_wrap(lua_State *L) {
    ssize_t stack_start = lua_gettop(L);

    lua_pushlightuserdata(L, (void *)&config_registry_keys.wrap);
    lua_rawget(L, LUA_REGISTRYINDEX);

    if (!luaL_testudata(L, -1, METATABLE_WRAP)) {
        lua_pop(L, 1);
        ww_assert(lua_gettop(L) == stack_start);

        return NULL;
    }

    struct wrap **wrap = lua_touserdata(L, -1);

    lua_pop(L, 1);
    ww_assert(lua_gettop(L) == stack_start);

    return *wrap;
}

static inline int
l_active_instance_wall(lua_State *L, struct wall *wall) {
    int id = wall->active_instance;

    if (id >= 0) {
        lua_pushinteger(L, id + 1);
    } else {
        lua_pushnil(L);
    }

    return 1;
}

static inline int
l_active_instance_wrap(lua_State *L, struct wrap *wrap) {
    lua_pushinteger(L, 1);
    return 1;
}

static int
l_active_instance(lua_State *L) {
    struct wall *wall = get_wall(L);
    if (wall) {
        return l_active_instance_wall(L, wall);
    }

    struct wrap *wrap = get_wrap(L);
    if (wrap) {
        return l_active_instance_wrap(L, wrap);
    }

    return luaL_error(L, STARTUP_ERRMSG("active_instance"));
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
l_get_active_res_wall(lua_State *L, struct wall *wall) {
    if (wall->active_instance == -1) {
        return luaL_error(L, "cannot get resolution without active instance");
    }

    lua_pushinteger(L, wall->active_res.w);
    lua_pushinteger(L, wall->active_res.h);
    return 2;
}

static int
l_get_active_res_wrap(lua_State *L, struct wrap *wrap) {
    lua_pushinteger(L, wrap->active_res.w);
    lua_pushinteger(L, wrap->active_res.h);
    return 2;
}

static int
l_get_active_res(lua_State *L) {
    struct wall *wall = get_wall(L);
    if (wall) {
        return l_get_active_res_wall(L, wall);
    }

    struct wrap *wrap = get_wrap(L);
    if (wrap) {
        return l_get_active_res_wrap(L, wrap);
    }

    return luaL_error(L, STARTUP_ERRMSG("get_active_res"));
}

static inline int
l_goto_wall_wall(lua_State *L, struct wall *wall) {
    bool ok = wall_lua_return(wall) == 0;
    if (!ok) {
        return luaL_error(L, "wall already active");
    }

    return 0;
}

static inline int
l_goto_wall_wrap(lua_State *L, struct wrap *wrap) {
    ww_log(LOG_WARN, WRAP_ERRMSG("goto_wall"));
    return 0;
}

static int
l_goto_wall(lua_State *L) {
    struct wall *wall = get_wall(L);
    if (wall) {
        return l_goto_wall_wall(L, wall);
    }

    struct wrap *wrap = get_wrap(L);
    if (wrap) {
        return l_goto_wall_wrap(L, wrap);
    }

    return luaL_error(L, STARTUP_ERRMSG("goto_wall"));
}

static inline int
l_hovered_wall(lua_State *L, struct wall *wall) {
    int id = wall_lua_get_hovered(wall);

    if (id >= 0) {
        lua_pushinteger(L, id + 1);
    } else {
        lua_pushnil(L);
    }

    return 1;
}

static inline int
l_hovered_wrap(lua_State *L, struct wrap *wrap) {
    lua_pushnil(L);
    return 1;
}

static int
l_hovered(lua_State *L) {
    struct wall *wall = get_wall(L);
    if (wall) {
        return l_hovered_wall(L, wall);
    }

    struct wrap *wrap = get_wrap(L);
    if (wrap) {
        return l_hovered_wrap(L, wrap);
    }

    return luaL_error(L, STARTUP_ERRMSG("hovered"));
}

static inline int
l_instance_wall(lua_State *L, struct wall *wall) {
    static const int ARG_INSTANCE = 1;
    static const int IDX_STATE = 2;

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

    int id = luaL_checkint(L, ARG_INSTANCE);
    luaL_argcheck(L, id >= 1 && id <= wall->num_instances, ARG_INSTANCE, "invalid instance");
    struct instance_state state = wall->instances[id - 1]->state;

    lua_newtable(L);

    lua_pushstring(L, "screen");
    lua_pushstring(L, screen_names[state.screen]);
    lua_rawset(L, IDX_STATE);

    if (state.screen == SCREEN_GENERATING || state.screen == SCREEN_PREVIEWING) {
        lua_pushstring(L, "percent");
        lua_pushinteger(L, state.data.percent);
        lua_rawset(L, IDX_STATE);
    } else if (state.screen == SCREEN_INWORLD) {
        lua_pushstring(L, "inworld");
        lua_pushstring(L, inworld_names[state.data.inworld]);
        lua_rawset(L, IDX_STATE);
    }

    lua_pushstring(L, "last_load");
    lua_pushinteger(L, timespec_ms(&state.last_load));
    lua_rawset(L, IDX_STATE);

    lua_pushstring(L, "last_preview");
    lua_pushinteger(L, timespec_ms(&state.last_preview));
    lua_rawset(L, IDX_STATE);

    ww_assert(lua_gettop(L) == IDX_STATE);
    return 1;
}

static inline int
l_instance_wrap(lua_State *L, struct wrap *wrap) {
    return luaL_error(L, WRAP_ERRMSG("instance"));
}

static int
l_instance(lua_State *L) {
    struct wall *wall = get_wall(L);
    if (wall) {
        return l_instance_wall(L, wall);
    }

    struct wrap *wrap = get_wrap(L);
    if (wrap) {
        return l_instance_wrap(L, wrap);
    }

    return luaL_error(L, STARTUP_ERRMSG("instance"));
}

static int
l_listen(lua_State *L) {
    static const int ARG_LISTENER = 1;
    static const int IDX_DUP_EVENTS = 2;
    static const int IDX_ARG_LISTENER_VAL = 3;
    static const int IDX_PREV_EVENTS = 3;
    static const int IDX_ARG_LISTENER_INSTALL = 4;

    luaL_argcheck(L, lua_istable(L, ARG_LISTENER), ARG_LISTENER, "expected table");

    // Copy the functions from the provided table (ARG_LISTENER) into a new table (IDX_EVENTS).
    const char *functions[] = {"death",         "install", "preview_percent",
                               "preview_start", "resize",  "spawn"};

    lua_newtable(L);
    for (size_t i = 0; i < STATIC_ARRLEN(functions); i++) {
        // LUA STACK:
        // - duplicate event listener table
        // - event listener table (argument)

        lua_pushstring(L, functions[i]);
        lua_rawget(L, ARG_LISTENER);

        switch (lua_type(L, IDX_ARG_LISTENER_VAL)) {
        case LUA_TFUNCTION:
            lua_pushstring(L, functions[i]);
            lua_pushvalue(L, IDX_ARG_LISTENER_VAL);
            lua_rawset(L, IDX_DUP_EVENTS);
            break;
        case LUA_TNIL:
            break;
        default:
            return luaL_error(L, "expected '%s' to be of type 'function', was '%s'", functions[i],
                              luaL_typename(L, IDX_ARG_LISTENER_VAL));
        }

        lua_pop(L, 1);
        ww_assert(lua_gettop(L) == IDX_DUP_EVENTS);
    }

    // Store the previous event listener table on the stack.
    lua_pushlightuserdata(L, (void *)&config_registry_keys.events);
    lua_rawget(L, LUA_REGISTRYINDEX);
    ww_assert(lua_gettop(L) == IDX_PREV_EVENTS);

    // Replace the previous event listener table with the duplicated event listener table.
    lua_pushlightuserdata(L, (void *)&config_registry_keys.events);
    lua_pushvalue(L, IDX_DUP_EVENTS);
    lua_rawset(L, LUA_REGISTRYINDEX);

    // Call the `install` event handler if this call to `listen` was not at config load time.
    if (get_wall(L)) {
        ssize_t stack_start = lua_gettop(L);

        lua_pushstring(L, "install");
        lua_rawget(L, ARG_LISTENER);
        ww_assert(lua_gettop(L) == IDX_ARG_LISTENER_INSTALL);

        switch (lua_type(L, IDX_ARG_LISTENER_INSTALL)) {
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

    // LUA STACK:
    // - previous event listener table
    // - duplicate event listener table
    // - event listener table (argument)
    //
    // It is fine that there is an extra value on the stack (the duplicate event listener table), as
    // per "Programming in Lua" 26.1:
    //     "Therefore, the function does not need to clear the stack before pushing its results.
    //     After it returns, Lua automatically removes whatever is in the stack below the results."
    ww_assert(lua_gettop(L) == IDX_PREV_EVENTS);

    return 1;
}

static inline int
l_num_instances_wall(lua_State *L, struct wall *wall) {
    lua_pushinteger(L, wall->num_instances);
    return 1;
}

static inline int
l_num_instances_wrap(lua_State *L, struct wrap *wrap) {
    lua_pushinteger(L, 1);
    return 1;
}

static int
l_num_instances(lua_State *L) {
    struct wall *wall = get_wall(L);
    if (wall) {
        return l_num_instances_wall(L, wall);
    }

    struct wrap *wrap = get_wrap(L);
    if (wrap) {
        return l_num_instances_wrap(L, wrap);
    }

    return luaL_error(L, STARTUP_ERRMSG("num_instances"));
}

static inline int
l_play_wall(lua_State *L, struct wall *wall) {
    static const int ARG_INSTANCE = 1;

    int id = luaL_checkint(L, ARG_INSTANCE);
    luaL_argcheck(L, id >= 1 && id <= wall->num_instances, ARG_INSTANCE, "invalid instance");

    bool ok = wall_lua_play(wall, id - 1) == 0;
    if (!ok) {
        return luaL_error(L, "instance %d already active", id);
    }

    return 0;
}

static inline int
l_play_wrap(lua_State *L, struct wrap *wrap) {
    ww_log(LOG_WARN, WRAP_ERRMSG("play"));
    return 0;
}

static int
l_play(lua_State *L) {
    struct wall *wall = get_wall(L);
    if (wall) {
        return l_play_wall(L, wall);
    }

    struct wrap *wrap = get_wrap(L);
    if (wrap) {
        return l_play_wrap(L, wrap);
    }

    return luaL_error(L, STARTUP_ERRMSG("play"));
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

static inline int
l_reset_wall(lua_State *L, struct wall *wall) {
    static const int ARG_TARGET = 1;
    static const int ARG_COUNT_RESETS = 2;

    bool count_resets = true;
    switch (lua_gettop(L)) {
    case 1:
        count_resets = true;
        break;
    case 2:
        luaL_checktype(L, ARG_COUNT_RESETS, LUA_TBOOLEAN);
        count_resets = lua_toboolean(L, ARG_COUNT_RESETS);
        break;
    default:
        return luaL_error(L, "expected 1 or 2 arguments, got %d", lua_gettop(L));
    }

    int idx_arg_target_key = lua_gettop(L) + 1;
    int idx_arg_target_val = lua_gettop(L) + 2;

    switch (lua_type(L, ARG_TARGET)) {
    case LUA_TNUMBER: {
        int id = luaL_checkinteger(L, ARG_TARGET);
        luaL_argcheck(L, id >= 1 && id <= wall->num_instances, ARG_TARGET, "invalid instance");

        if (wall_lua_reset_one(wall, count_resets, id - 1) == 0) {
            lua_pushinteger(L, 1);
        } else {
            lua_pushinteger(L, 0);
        }

        return 1;
    }
    case LUA_TTABLE: {
        size_t n = lua_objlen(L, ARG_TARGET);
        if (n == 0) {
            lua_pushinteger(L, 0);
            return 1;
        }

        int *ids = zalloc(n, sizeof(int));

        lua_pushnil(L);
        for (size_t i = 0; i < n; i++) {
            ww_assert(lua_next(L, ARG_TARGET) != 0);
            ww_assert(lua_gettop(L) == idx_arg_target_val);

            if (!lua_isnumber(L, idx_arg_target_val)) {
                free(ids);
                return luaL_error(L, "expected instance ID (number), got %s", luaL_typename(L, -1));
            }
            int id = lua_tointeger(L, idx_arg_target_val);
            if (id < 1 || id > wall->num_instances) {
                free(ids);
                return luaL_error(L, "invalid instance: %d", id);
            }

            ids[i] = id - 1;

            lua_pop(L, 1);
            ww_assert(lua_gettop(L) == idx_arg_target_key);
        }

        lua_pushinteger(L, wall_lua_reset_many(wall, count_resets, n, ids));
        free(ids);
        return 1;
    }
    default:
        return luaL_argerror(L, ARG_TARGET, "expected number or table");
    }
}

static inline int
l_reset_wrap(lua_State *L, struct wrap *wrap) {
    return luaL_error(L, WRAP_ERRMSG("reset"));
}

static int
l_reset(lua_State *L) {
    struct wall *wall = get_wall(L);
    if (wall) {
        return l_reset_wall(L, wall);
    }

    struct wrap *wrap = get_wrap(L);
    if (wrap) {
        return l_reset_wrap(L, wrap);
    }

    return luaL_error(L, STARTUP_ERRMSG("reset"));
}

static inline int
l_set_keymap_wall(lua_State *L, struct wall *wall) {
    static const int ARG_KEYMAP = 1;

    luaL_argcheck(L, lua_istable(L, ARG_KEYMAP), ARG_KEYMAP, "expected table");

    const struct xkb_rule_names rule_names = get_rule_names(L);
    server_seat_lua_set_keymap(wall->server->seat, &rule_names);

    return 0;
}

static inline int
l_set_keymap_wrap(lua_State *L, struct wrap *wrap) {
    static const int ARG_KEYMAP = 1;

    luaL_argcheck(L, lua_istable(L, ARG_KEYMAP), ARG_KEYMAP, "expected table");

    const struct xkb_rule_names rule_names = get_rule_names(L);
    server_seat_lua_set_keymap(wrap->server->seat, &rule_names);

    return 0;
}

static int
l_set_keymap(lua_State *L) {
    struct wall *wall = get_wall(L);
    if (wall) {
        return l_set_keymap_wall(L, wall);
    }

    struct wrap *wrap = get_wrap(L);
    if (wrap) {
        return l_set_keymap_wrap(L, wrap);
    }

    return luaL_error(L, STARTUP_ERRMSG("set_keymap"));
}

static inline int
l_set_layout_wall(lua_State *L, struct wall *wall) {
    static const int ARG_LAYOUT = 1;

    luaL_argcheck(L, lua_istable(L, ARG_LAYOUT), ARG_LAYOUT, "expected table");

    lua_pushlightuserdata(L, (void *)&config_registry_keys.layout);
    lua_pushvalue(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);

    return 0;
}

static inline int
l_set_layout_wrap(lua_State *L, struct wrap *wrap) {
    ww_log(LOG_WARN, WRAP_ERRMSG("set_layout"));
    return 0;
}

static int
l_set_layout(lua_State *L) {
    struct wall *wall = get_wall(L);
    if (wall) {
        return l_set_layout_wall(L, wall);
    }

    struct wrap *wrap = get_wrap(L);
    if (wrap) {
        return l_set_layout_wrap(L, wrap);
    }

    return luaL_error(L, STARTUP_ERRMSG("set_layout"));
}

static inline int
l_set_priority_wall(lua_State *L, struct wall *wall) {
    static const int ARG_INSTANCE = 1;
    static const int ARG_PRIORITY = 2;

    int id = luaL_checkint(L, ARG_INSTANCE);
    luaL_checktype(L, ARG_PRIORITY, LUA_TBOOLEAN);
    bool priority = lua_toboolean(L, ARG_PRIORITY);
    luaL_argcheck(L, id >= 1 && id <= wall->num_instances, ARG_INSTANCE, "invalid instance");

    if (!wall->cpu) {
        return 0;
    }

    cpu_set_priority(wall->cpu, id - 1, priority);
    return 0;
}

static inline int
l_set_priority_wrap(lua_State *L, struct wrap *wrap) {
    ww_log(LOG_WARN, WRAP_ERRMSG("set_priority"));
    return 0;
}

static int
l_set_priority(lua_State *L) {
    struct wall *wall = get_wall(L);
    if (wall) {
        return l_set_priority_wall(L, wall);
    }

    struct wrap *wrap = get_wrap(L);
    if (wrap) {
        return l_set_priority_wrap(L, wrap);
    }

    return luaL_error(L, STARTUP_ERRMSG("set_priority"));
}

static inline int
l_set_resolution_wall(lua_State *L, struct wall *wall) {
    static const int ARG_INSTANCE = 1;
    static const int ARG_WIDTH = 2;
    static const int ARG_HEIGHT = 3;

    int id = luaL_checkint(L, ARG_INSTANCE);
    int32_t width = luaL_checkint(L, ARG_WIDTH);
    int32_t height = luaL_checkint(L, ARG_HEIGHT);

    luaL_argcheck(L, id >= 1 && id <= wall->num_instances, ARG_INSTANCE, "invalid instance");
    luaL_argcheck(L, width >= 0, ARG_WIDTH, "width must be non-negative");
    luaL_argcheck(L, height >= 0, ARG_HEIGHT, "height must be non-negative");
    bool ok = wall_lua_set_res(wall, id - 1, width, height) == 0;
    if (!ok) {
        return luaL_error(L, "cannot set resolution");
    }
    return 0;
}

static inline int
l_set_resolution_wrap(lua_State *L, struct wrap *wrap) {
    static const int ARG_INSTANCE = 1;
    static const int ARG_WIDTH = 2;
    static const int ARG_HEIGHT = 3;

    int id = luaL_checkint(L, ARG_INSTANCE);
    int32_t width = luaL_checkint(L, ARG_WIDTH);
    int32_t height = luaL_checkint(L, ARG_HEIGHT);

    luaL_argcheck(L, id == 1, ARG_INSTANCE, "invalid instance");
    luaL_argcheck(L, width >= 0, ARG_WIDTH, "width must be non-negative");
    luaL_argcheck(L, height >= 0, ARG_HEIGHT, "height must be non-negative");
    bool ok = wrap_lua_set_res(wrap, width, height) == 0;
    if (!ok) {
        return luaL_error(L, "cannot set resolution");
    }
    return 0;
}

static int
l_set_resolution(lua_State *L) {
    struct wall *wall = get_wall(L);
    if (wall) {
        return l_set_resolution_wall(L, wall);
    }

    struct wrap *wrap = get_wrap(L);
    if (wrap) {
        return l_set_resolution_wrap(L, wrap);
    }

    return luaL_error(L, STARTUP_ERRMSG("set_resolution"));
}

static inline int
l_set_sensitivity_wall(lua_State *L, struct wall *wall) {
    static const int ARG_SENS = 1;

    double sens = luaL_checknumber(L, ARG_SENS);
    luaL_argcheck(L, sens > 0, ARG_SENS, "sensitivity must be a positive, non-zero number");

    server_relative_pointer_set_sens(wall->server->relative_pointer, sens);
    return 0;
}

static inline int
l_set_sensitivity_wrap(lua_State *L, struct wrap *wrap) {
    static const int ARG_SENS = 1;

    double sens = luaL_checknumber(L, ARG_SENS);
    luaL_argcheck(L, sens > 0, ARG_SENS, "sensitivity must be a positive, non-zero number");

    server_relative_pointer_set_sens(wrap->server->relative_pointer, sens);
    return 0;
}

static int
l_set_sensitivity(lua_State *L) {
    struct wall *wall = get_wall(L);
    if (wall) {
        return l_set_sensitivity_wall(L, wall);
    }

    struct wrap *wrap = get_wrap(L);
    if (wrap) {
        return l_set_sensitivity_wrap(L, wrap);
    }

    return luaL_error(L, STARTUP_ERRMSG("set_sensitivity"));
}

static inline int
l_window_size_wall(lua_State *L, struct wall *wall) {
    if (!wall->server->ui->mapped) {
        lua_pushinteger(L, 0);
        lua_pushinteger(L, 0);
    } else {
        lua_pushinteger(L, wall->width);
        lua_pushinteger(L, wall->height);
    }
    return 2;
}

static inline int
l_window_size_wrap(lua_State *L, struct wrap *wrap) {
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
l_window_size(lua_State *L) {
    struct wall *wall = get_wall(L);
    if (wall) {
        return l_window_size_wall(L, wall);
    }

    struct wrap *wrap = get_wrap(L);
    if (wrap) {
        return l_window_size_wrap(L, wrap);
    }

    return luaL_error(L, STARTUP_ERRMSG("window_size"));
}

static int
l_getenv(lua_State *L) {
    static const int ARG_ENV = 1;

    const char *var = luaL_checkstring(L, ARG_ENV);
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
    {"get_active_res", l_get_active_res},
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
    ww_assert(lua_gettop(cfg->L) == 0);

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
config_api_set_wall(struct config *cfg, struct wall *wall) {
    ww_assert(lua_gettop(cfg->L) == 0);

    struct wall **udata = lua_newuserdata(cfg->L, sizeof(*udata));
    luaL_getmetatable(cfg->L, METATABLE_WALL);
    lua_setmetatable(cfg->L, -2);
    *udata = wall;

    lua_pushlightuserdata(cfg->L, (void *)&config_registry_keys.wall);
    lua_pushvalue(cfg->L, -2);
    lua_rawset(cfg->L, LUA_REGISTRYINDEX);

    lua_pop(cfg->L, 1);
    ww_assert(lua_gettop(cfg->L) == 0);
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
