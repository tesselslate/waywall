#ifndef WAYWALL_CONFIG_INTERNAL_H
#define WAYWALL_CONFIG_INTERNAL_H

#include "config/action.h"

#define BIND_BUFLEN 17
#define METATABLE_WALL "waywall.wall"

struct lua_State;

extern const struct config_registry_keys {
    char actions;
    char layout;
    char layout_reason;
    char wall;
} config_registry_keys;

void config_dump_stack(struct lua_State *L);
void config_encode_bind(char buf[static BIND_BUFLEN], struct config_action action);

#endif
