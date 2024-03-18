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
    } type;
    union {
        int instance;
    } data;

    int32_t x, y, w, h;
};

void config_layout_destroy(struct config_layout *layout);
struct config_layout *config_layout_request_death(struct config *cfg, struct wall *wall, int id);
struct config_layout *config_layout_request_manual(struct config *cfg, struct wall *wall);
struct config_layout *config_layout_request_preview_percent(struct config *cfg, struct wall *wall,
                                                            int id, int percent);
struct config_layout *config_layout_request_preview_start(struct config *cfg, struct wall *wall,
                                                          int id);
struct config_layout *config_layout_request_resize(struct config *cfg, struct wall *wall, int width,
                                                   int height);
struct config_layout *config_layout_request_spawn(struct config *cfg, struct wall *wall, int id);

#endif
