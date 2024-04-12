#include "config/internal.h"
#include "config/config.h"
#include <luajit-2.1/lauxlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

const struct config_registry_keys config_registry_keys = {0};

#define MAX_INSTRUCTIONS 50000000

static void
pcall_hook(struct lua_State *L, struct lua_Debug *dbg) {
    luaL_error(L, "instruction count exceeded");
}

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
    if (cfg->use_hook) {
        lua_sethook(cfg->L, pcall_hook, LUA_MASKCOUNT, MAX_INSTRUCTIONS);
    }

    int ret = lua_pcall(cfg->L, nargs, nresults, errfunc);

    if (cfg->use_hook) {
        lua_sethook(cfg->L, NULL, 0, 0);
    }

    return ret;
}
