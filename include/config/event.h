#ifndef WAYWALL_CONFIG_EVENT_H
#define WAYWALL_CONFIG_EVENT_H

#include <stddef.h>
#include <stdint.h>

struct config;
struct wall;

void config_signal_death(struct config *cfg, struct wall *wall, int id);
void config_signal_preview_percent(struct config *cfg, struct wall *wall, int id, int percent);
void config_signal_preview_start(struct config *cfg, struct wall *wall, int id);
void config_signal_resize(struct config *cfg, struct wall *wall, int width, int height);
void config_signal_spawn(struct config *cfg, struct wall *wall, int id);

#endif
