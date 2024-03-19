#include "config/layout.h"
#include "config/config.h"
#include "config/internal.h"
#include "util.h"
#include "wall.h"
#include <luajit-2.1/lauxlib.h>
#include <luajit-2.1/lua.h>
#include <luajit-2.1/lualib.h>
#include <stddef.h>
#include <stdlib.h>

static int
get_int32(lua_State *L, const char *key, int32_t *dst) {
    lua_pushstring(L, key);
    lua_rawget(L, -2);

    if (lua_type(L, -1) == LUA_TNUMBER) {
        double x = lua_tonumber(L, -1);
        int32_t ix = (int32_t)x;
        if (ix != x) {
            ww_log(LOG_ERROR, "expected key '%s' of layout element to be an integer, got '%lf'",
                   key, x);
            return 1;
        }
        *dst = ix;
    } else {
        ww_log(LOG_ERROR, "expected key '%s' of layout element to be of type 'number', got '%s'",
               key, luaL_typename(L, -1));
        return 1;
    }

    lua_pop(L, 1);
    return 0;
}

static int
unmarshal_layout_element(struct config *cfg, struct wall *wall,
                         struct config_layout_element *elem) {
    if (!lua_istable(cfg->L, -1)) {
        ww_log(LOG_ERROR, "expected to receive layout element of type 'table', got '%s'",
               luaL_typename(cfg->L, -1));
        return 1;
    }

    // Element type
    lua_pushinteger(cfg->L, 1);
    lua_rawget(cfg->L, -2);
    if (lua_type(cfg->L, -1) == LUA_TSTRING) {
        const char *type = lua_tostring(cfg->L, -1);
        if (strcmp(type, "instance") == 0) {
            elem->type = LAYOUT_ELEMENT_INSTANCE;
        } else {
            ww_log(LOG_ERROR, "received layout element of unknown type '%s'", type);
            return 1;
        }
    } else {
        ww_log(LOG_ERROR, "expected key '1' of layout element to be of type 'string', got '%s'",
               luaL_typename(cfg->L, -1));
        return 1;
    }
    lua_pop(cfg->L, 1);

    // Type-specific data
    lua_pushinteger(cfg->L, 2);
    lua_rawget(cfg->L, -2);
    switch (elem->type) {
    case LAYOUT_ELEMENT_INSTANCE:
        if (lua_type(cfg->L, -1) == LUA_TNUMBER) {
            int id = lua_tointeger(cfg->L, -1);
            if (id < 1 || id > wall->num_instances) {
                ww_log(LOG_ERROR, "layout element contains invalid instance %d", id);
                return 1;
            }
            elem->data.instance = id - 1;
        } else {
            ww_log(LOG_ERROR,
                   "expected key '2' of instance layout element to be of type 'number', got '%s'",
                   luaL_typename(cfg->L, -1));
            return 1;
        }
    }
    lua_pop(cfg->L, 1);

    // Common properties
    if (get_int32(cfg->L, "x", &elem->x) != 0) {
        return 1;
    }
    if (get_int32(cfg->L, "y", &elem->y) != 0) {
        return 1;
    }
    if (get_int32(cfg->L, "w", &elem->w) != 0) {
        return 1;
    }
    if (get_int32(cfg->L, "h", &elem->h) != 0) {
        return 1;
    }

    return 0;
}

static struct config_layout *
unmarshal_layout(struct config *cfg, struct wall *wall) {
    if (!lua_istable(cfg->L, -1)) {
        ww_log(LOG_ERROR, "expected to receive layout of type 'table', got '%s'",
               luaL_typename(cfg->L, -1));
        return NULL;
    }

    struct config_layout *layout = calloc(1, sizeof(*layout));
    if (!layout) {
        ww_log(LOG_ERROR, "failed to allocate config_layout");
        return NULL;
    }

    layout->num_elements = lua_objlen(cfg->L, -1);
    if (layout->num_elements == 0) {
        return layout;
    }

    layout->elements = calloc(layout->num_elements, sizeof(*layout->elements));
    if (!layout->elements) {
        ww_log(LOG_ERROR, "failed to allocate config_layout->entries");
        goto fail_elements;
    }

    size_t i = 0;
    lua_pushnil(cfg->L);
    while (lua_next(cfg->L, -2)) {
        struct config_layout_element *element = &layout->elements[i++];
        if (unmarshal_layout_element(cfg, wall, element) != 0) {
            goto fail_element;
        }
        lua_pop(cfg->L, 1);
    }
    lua_pop(cfg->L, 1);

    return layout;

fail_element:
    lua_pop(cfg->L, 2);
    free(layout->elements);

fail_elements:
    free(layout);
    return NULL;
}

void
config_layout_destroy(struct config_layout *layout) {
    if (layout->elements) {
        free(layout->elements);
    }
    free(layout);
}

struct config_layout *
config_layout_request_death(struct config *cfg, struct wall *wall, int id) {
    if (!cfg->layout.handle_death) {
        return NULL;
    }

    lua_pushlightuserdata(cfg->L, (void *)&config_registry_keys.layout);
    lua_rawget(cfg->L, LUA_REGISTRYINDEX);
    lua_pushstring(cfg->L, "death");
    lua_rawget(cfg->L, -2);

    ww_assert(lua_isfunction(cfg->L, -1));
    lua_pushinteger(cfg->L, id);
    if (lua_pcall(cfg->L, 1, 1, 0) != 0) {
        ww_log(LOG_ERROR, "failed to call layout.death: '%s'", lua_tostring(cfg->L, -1));

        lua_settop(cfg->L, 0);
        return NULL;
    }

    struct config_layout *layout = unmarshal_layout(cfg, wall);

    lua_settop(cfg->L, 0);
    return layout;
}

struct config_layout *
config_layout_request_manual(struct config *cfg, struct wall *wall) {
    if (!cfg->layout.handle_manual) {
        return NULL;
    }

    lua_pushlightuserdata(cfg->L, (void *)&config_registry_keys.layout_reason);
    lua_rawget(cfg->L, LUA_REGISTRYINDEX);

    if (lua_isnil(cfg->L, -1)) {
        lua_pop(cfg->L, 1);
        return NULL;
    }

    lua_pushlightuserdata(cfg->L, (void *)&config_registry_keys.layout);
    lua_rawget(cfg->L, LUA_REGISTRYINDEX);
    lua_pushstring(cfg->L, "manual");
    lua_rawget(cfg->L, -2);

    ww_assert(lua_isfunction(cfg->L, -1));
    lua_pushvalue(cfg->L, -2);
    if (lua_pcall(cfg->L, 1, 1, 0) != 0) {
        ww_log(LOG_ERROR, "failed to call layout.manual: '%s'", lua_tostring(cfg->L, -1));

        lua_settop(cfg->L, 0);
        return NULL;
    }

    struct config_layout *layout = unmarshal_layout(cfg, wall);

    lua_settop(cfg->L, 0);
    return layout;
}

struct config_layout *
config_layout_request_preview_percent(struct config *cfg, struct wall *wall, int id, int percent) {
    if (!cfg->layout.handle_preview_percent) {
        return NULL;
    }

    lua_pushlightuserdata(cfg->L, (void *)&config_registry_keys.layout);
    lua_rawget(cfg->L, LUA_REGISTRYINDEX);
    lua_pushstring(cfg->L, "preview_percent");
    lua_rawget(cfg->L, -2);

    ww_assert(lua_isfunction(cfg->L, -1));
    lua_pushinteger(cfg->L, id);
    lua_pushinteger(cfg->L, percent);
    if (lua_pcall(cfg->L, 2, 1, 0) != 0) {
        ww_log(LOG_ERROR, "failed to call layout.preview_percent: '%s'", lua_tostring(cfg->L, -1));

        lua_settop(cfg->L, 0);
        return NULL;
    }

    struct config_layout *layout = unmarshal_layout(cfg, wall);

    lua_settop(cfg->L, 0);
    return layout;
}

struct config_layout *
config_layout_request_preview_start(struct config *cfg, struct wall *wall, int id) {
    if (!cfg->layout.handle_preview_start) {
        return NULL;
    }

    lua_pushlightuserdata(cfg->L, (void *)&config_registry_keys.layout);
    lua_rawget(cfg->L, LUA_REGISTRYINDEX);
    lua_pushstring(cfg->L, "preview_start");
    lua_rawget(cfg->L, -2);

    ww_assert(lua_isfunction(cfg->L, -1));
    lua_pushinteger(cfg->L, id);
    if (lua_pcall(cfg->L, 1, 1, 0) != 0) {
        ww_log(LOG_ERROR, "failed to call layout.preview_start: '%s'", lua_tostring(cfg->L, -1));

        lua_settop(cfg->L, 0);
        return NULL;
    }

    struct config_layout *layout = unmarshal_layout(cfg, wall);

    lua_settop(cfg->L, 0);
    return layout;
}

struct config_layout *
config_layout_request_resize(struct config *cfg, struct wall *wall, int width, int height) {
    if (!cfg->layout.handle_resize) {
        return NULL;
    }

    lua_pushlightuserdata(cfg->L, (void *)&config_registry_keys.layout);
    lua_rawget(cfg->L, LUA_REGISTRYINDEX);
    lua_pushstring(cfg->L, "resize");
    lua_rawget(cfg->L, -2);

    ww_assert(lua_isfunction(cfg->L, -1));
    lua_pushinteger(cfg->L, width);
    lua_pushinteger(cfg->L, height);
    if (lua_pcall(cfg->L, 2, 1, 0) != 0) {
        ww_log(LOG_ERROR, "failed to call layout.resize: '%s'", lua_tostring(cfg->L, -1));

        lua_settop(cfg->L, 0);
        return NULL;
    }

    struct config_layout *layout = unmarshal_layout(cfg, wall);

    lua_settop(cfg->L, 0);
    return layout;
}

struct config_layout *
config_layout_request_spawn(struct config *cfg, struct wall *wall, int id) {
    if (!cfg->layout.handle_spawn) {
        return NULL;
    }

    lua_pushlightuserdata(cfg->L, (void *)&config_registry_keys.layout);
    lua_rawget(cfg->L, LUA_REGISTRYINDEX);
    lua_pushstring(cfg->L, "spawn");
    lua_rawget(cfg->L, -2);

    ww_assert(lua_isfunction(cfg->L, -1));
    lua_pushinteger(cfg->L, id);
    if (lua_pcall(cfg->L, 1, 1, 0) != 0) {
        ww_log(LOG_ERROR, "failed to call layout.spawn: '%s'", lua_tostring(cfg->L, -1));

        lua_settop(cfg->L, 0);
        return NULL;
    }

    struct config_layout *layout = unmarshal_layout(cfg, wall);

    lua_settop(cfg->L, 0);
    return layout;
}
