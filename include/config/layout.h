#ifndef WAYWALL_CONFIG_LAYOUT_H
#define WAYWALL_CONFIG_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

struct config;
struct wall;

struct config_layout {
    struct config_layout_element *elements;
    size_t num_elements;
};

struct config_layout_element {
    enum {
        LAYOUT_ELEMENT_INSTANCE,
        LAYOUT_ELEMENT_RECTANGLE,
    } type;
    union {
        int instance;
        uint8_t rectangle[4];
    } data;

    int32_t x, y, w, h;
};

void config_layout_destroy(struct config_layout *layout);
struct config_layout *config_layout_get(struct config *config, struct wall *wall);

#endif
