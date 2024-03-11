#ifndef WAYWALL_CONFIG_H
#define WAYWALL_CONFIG_H

struct config_vm {
    struct lua_State *L;
};

struct config {
    struct {
        char *theme;
        char *icon;
        int size;
    } cursor;
    struct {
        int width, height;
        int stretch_width, stretch_height;
    } wall;

    struct config_vm vm;
};

struct config *config_create();
void config_destroy(struct config *cfg);
char *config_get_path();
int config_populate(struct config *cfg);

#endif
