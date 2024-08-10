#ifndef WAYWALL_CONFIG_API_H
#define WAYWALL_CONFIG_API_H

#include "config/config.h"
#include "wrap.h"

int config_api_init(struct config *cfg, const char *profile);
void config_api_set_wrap(struct config *cfg, struct wrap *wrap);
void config_api_signal(struct config *cfg, const char *signal);

#endif
