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
    char events;
    char layout;
    char profile;
    char wall;
    char wrap;
} config_registry_keys;

void config_dump_stack(lua_State *L);
void config_encode_bind(char buf[static BIND_BUFLEN], struct config_action action);
int config_parse_hex(uint8_t rgba[static 4], const char *raw);
int config_pcall(struct config *cfg, int nargs, int nresults, int errfunc);

#endif
