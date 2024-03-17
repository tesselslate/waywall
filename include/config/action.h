#ifndef WAYWALL_CONFIG_ACTION_H
#define WAYWALL_CONFIG_ACTION_H

#include <stdint.h>

struct config;
struct wall;

struct config_action {
    enum config_action_type {
        CONFIG_ACTION_NONE,
        CONFIG_ACTION_BUTTON,
        CONFIG_ACTION_KEY,
    } type;

    uint32_t data;
    uint32_t modifiers;
};

int config_action_try(struct config *config, struct wall *wall, struct config_action action);

#endif
