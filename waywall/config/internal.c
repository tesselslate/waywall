#include "config/internal.h"
#include <luajit-2.1/lauxlib.h>
#include <stddef.h>
#include <stdio.h>

const struct config_registry_keys config_registry_keys = {0};

// This function is intended for debugging purposes.
// Adapted from: https://stackoverflow.com/a/59097940
void
config_dump_stack(struct lua_State *L) {
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
