#ifndef WAYWALL_CONFIG_API_H
#define WAYWALL_CONFIG_API_H

struct config;
struct wall;

int config_api_init(struct config *cfg);
void config_api_set_wall(struct config *cfg, struct wall *wall);

#endif
