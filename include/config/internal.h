#pragma once

#include "config/config.h"
#include <luajit-2.1/lua.h>
#include <stdint.h>

int config_api_init(struct config_vm *vm);

void config_dump_stack(lua_State *L);
int config_parse_hex(uint8_t rgba[static 4], const char *raw);
