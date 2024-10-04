#ifndef WAYWALL_CONFIG_INTERNAL_H
#define WAYWALL_CONFIG_INTERNAL_H

#include "config/action.h"
#include "config/config.h"
#include <luajit-2.1/lua.h>
#include <stdint.h>

#define BIND_BUFLEN 17
#define METATABLE_WALL "waywall.wall"
#define METATABLE_WRAP "waywall.wrap"

extern const struct config_registry_keys {
    char actions;
    char coroutines;
    char events;
    char layout;
    char profile;
    char wall;
    char wrap;
} config_registry_keys;

struct config_coro {
    struct wl_list link; // config.coroutines
    lua_State *L;

    struct config *parent;
};

struct config_coro *config_coro_add(lua_State *coro);
void config_coro_delete(struct config_coro *ccoro);
struct config_coro *config_coro_lookup(lua_State *coro);
void config_dump_stack(lua_State *L);
void config_encode_bind(char buf[static BIND_BUFLEN], struct config_action action);
struct wrap *config_get_wrap(lua_State *L);
int config_parse_hex(uint8_t rgba[static 4], const char *raw);
int config_pcall(struct config *cfg, int nargs, int nresults, int errfunc);

#endif
