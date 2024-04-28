#ifndef WAYWALL_CONFIG_API_H
#define WAYWALL_CONFIG_API_H

#include "config/config.h"
#include "wall.h"
#include "wrap.h"

int config_api_init(struct config *cfg, const char *profile);
void config_api_set_wall(struct config *cfg, struct wall *wall);
void config_api_set_wrap(struct config *cfg, struct wrap *wrap);

#endif
