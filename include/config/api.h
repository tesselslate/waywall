#ifndef WAYWALL_CONFIG_API_H
#define WAYWALL_CONFIG_API_H

struct config;
struct wall;
struct wrap;

int config_api_init(struct config *cfg, const char *profile);
void config_api_set_wall(struct config *cfg, struct wall *wall);
void config_api_set_wrap(struct config *cfg, struct wrap *wrap);

#endif
