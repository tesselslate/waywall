#include "config/internal.h"
#include "config/action.h"
#include "config/config.h"
#include "util/alloc.h"
#include "util/prelude.h"
#include "wrap.h"
#include <luajit-2.1/lauxlib.h>
#include <luajit-2.1/lua.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

const struct config_registry_keys config_registry_keys = {0};

#define MAX_INSTRUCTIONS 50000000

static void
pcall_hook(lua_State *L, struct lua_Debug *dbg) {
    luaL_error(L, "instruction count exceeded");
}

struct config_coro *
config_coro_add(lua_State *coro) {
    ssize_t stack_start = lua_gettop(coro);

    struct wrap *wrap = config_get_wrap(coro);
    ww_assert(wrap);

    struct config_coro *ccoro = zalloc(1, sizeof(*ccoro));
    ccoro->parent = wrap->cfg;
    ccoro->L = coro;

    // Lua coroutines are garbage collected and do not have an explicit destructor in the C API.
    // Thus, we need to store the coroutine in a global table.
    lua_pushlightuserdata(coro, (void *)&config_registry_keys.coroutines);
    lua_gettable(coro, LUA_REGISTRYINDEX);
    lua_pushthread(coro);
    lua_pushlightuserdata(coro, ccoro);
    lua_rawset(coro, -3);
    lua_pop(coro, 1);

    ww_assert(lua_gettop(coro) == stack_start);

    wl_list_insert(&wrap->cfg->coroutines, &ccoro->link);
    return ccoro;
}

void
config_coro_delete(struct config_coro *ccoro) {
    // Only remove the coroutine from the global coroutines table if the owning Lua VM is still
    // alive.
    if (ccoro->parent) {
        lua_pushlightuserdata(ccoro->L, (void *)&config_registry_keys.coroutines);
        lua_gettable(ccoro->L, LUA_REGISTRYINDEX);
        lua_pushthread(ccoro->L);
        lua_pushnil(ccoro->L);
        lua_rawset(ccoro->L, -3);
    }

    wl_list_remove(&ccoro->link);
    free(ccoro);
}

struct config_coro *
config_coro_lookup(lua_State *coro) {
    // config_coro_lookup MUST NOT be called if the owning Lua VM has already been closed (i.e. it
    // cannot be called from timer callbacks, since the owning VM may have already been destroyed.)
    struct wrap *wrap = config_get_wrap(coro);
    ww_assert(wrap);

    struct config_coro *ccoro;
    wl_list_for_each (ccoro, &wrap->cfg->coroutines, link) {
        if (ccoro->L == coro) {
            return ccoro;
        }
    }

    return NULL;
}

// This function is intended for debugging purposes.
// Adapted from: https://stackoverflow.com/a/59097940
void
config_dump_stack(lua_State *L) {
    int n = lua_gettop(L);
    fprintf(stderr, "--- stack (%d)\n", n);

    for (int i = 1; i <= n; i++) {
        fprintf(stderr, "%d\t%s\t", i, luaL_typename(L, i));

        switch (lua_type(L, i)) {
        case LUA_TBOOLEAN:
            fprintf(stderr, lua_toboolean(L, i) ? "true\n" : "false\n");
            break;
        case LUA_TNUMBER:
            fprintf(stderr, "%lf\n", lua_tonumber(L, i));
            break;
        case LUA_TSTRING:
            fprintf(stderr, "%s\n", lua_tostring(L, i));
            break;
        default:
            fprintf(stderr, "%p\n", lua_topointer(L, i));
            break;
        }
    }
}

void
config_encode_bind(char buf[static BIND_BUFLEN], struct config_action action) {
    uint64_t data = (((uint64_t)action.data) << 32) | (uint64_t)action.modifiers;

    buf[0] = (action.type == CONFIG_ACTION_BUTTON) ? 'm' : 'k';
    for (size_t i = 0; i < 16; i++) {
        buf[i + 1] = "0123456789abcdef"[(data >> (i * 4)) & 0xF];
    }
}

struct wrap *
config_get_wrap(lua_State *L) {
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

int
config_parse_hex(uint8_t rgba[static 4], const char *raw) {
    ssize_t len = strlen(raw);
    bool maybe_valid_rgb = (len == 6) || (len == 7 && raw[0] == '#');
    bool maybe_valid_rgba = (len == 8) || (len == 9 && raw[0] == '#');
    if (!maybe_valid_rgb && !maybe_valid_rgba) {
        return 1;
    }

    unsigned int r = 0, g = 0, b = 0, a = 255;
    if (maybe_valid_rgb) {
        ssize_t n = sscanf(raw[0] == '#' ? raw + 1 : raw, "%02x%02x%02x", &r, &g, &b);
        if (n != 3) {
            return 1;
        }
    } else {
        ssize_t n = sscanf(raw[0] == '#' ? raw + 1 : raw, "%02x%02x%02x%02x", &r, &g, &b, &a);
        if (n != 4) {
            return 1;
        }
    }

    rgba[0] = r;
    rgba[1] = g;
    rgba[2] = b;
    rgba[3] = a;

    return 0;
}

int
config_pcall(struct config *cfg, int nargs, int nresults, int errfunc) {
    if (!cfg->experimental.jit) {
        lua_sethook(cfg->L, pcall_hook, LUA_MASKCOUNT, MAX_INSTRUCTIONS);
    }

    int ret = lua_pcall(cfg->L, nargs, nresults, errfunc);

    if (!cfg->experimental.jit) {
        lua_sethook(cfg->L, NULL, 0, 0);
    }

    return ret;
}
