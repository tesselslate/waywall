#include "config/layout.h"
#include "config/config.h"
#include "config/internal.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include "wall.h"
#include <luajit-2.1/lauxlib.h>
#include <luajit-2.1/lua.h>
#include <stdlib.h>
#include <string.h>

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
        } else if (strcmp(type, "rectangle") == 0) {
            elem->type = LAYOUT_ELEMENT_RECTANGLE;
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
        break;
    case LAYOUT_ELEMENT_RECTANGLE:
        if (lua_type(cfg->L, -1) == LUA_TSTRING) {
            const char *raw_rgba = lua_tostring(cfg->L, -1);
            if (config_parse_hex(elem->data.rectangle, raw_rgba) != 0) {
                ww_log(LOG_ERROR, "layout element contains invalid hex color '%s'", raw_rgba);
                return 1;
            }
        } else {
            ww_log(LOG_ERROR,
                   "expected key '2' of rectangle layout element to be of type 'string', got '%s'",
                   luaL_typename(cfg->L, -1));
            return 1;
        }
        break;
    }
    lua_pop(cfg->L, 1);

    // Common properties
    if (get_int32(cfg->L, "x", &elem->x) != 0) {
        return 1;
    }
    if (elem->x < 0) {
        ww_log(LOG_ERROR, "x of element must be greater than or equal to zero");
        return 1;
    }

    if (get_int32(cfg->L, "y", &elem->y) != 0) {
        return 1;
    }
    if (elem->y < 0) {
        ww_log(LOG_ERROR, "y of element must be greater than or equal to zero");
        return 1;
    }

    if (get_int32(cfg->L, "w", &elem->w) != 0) {
        return 1;
    }
    if (elem->w <= 0) {
        ww_log(LOG_ERROR, "width of element must be greater than zero");
        return 1;
    }

    if (get_int32(cfg->L, "h", &elem->h) != 0) {
        return 1;
    }
    if (elem->h <= 0) {
        ww_log(LOG_ERROR, "height of element must be greater than zero");
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

    struct config_layout *layout = zalloc(1, sizeof(*layout));

    layout->num_elements = lua_objlen(cfg->L, -1);
    if (layout->num_elements == 0) {
        return layout;
    }

    layout->elements = zalloc(layout->num_elements, sizeof(*layout->elements));

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
config_layout_get(struct config *cfg, struct wall *wall) {
    ww_assert(lua_gettop(cfg->L) == 0);

    lua_pushlightuserdata(cfg->L, (void *)&config_registry_keys.layout);
    lua_rawget(cfg->L, LUA_REGISTRYINDEX);

    if (lua_isnil(cfg->L, -1)) {
        lua_pop(cfg->L, 1);
        ww_assert(lua_gettop(cfg->L) == 0);

        return NULL;
    }

    ww_assert(lua_istable(cfg->L, -1));
    struct config_layout *layout = unmarshal_layout(cfg, wall);

    lua_settop(cfg->L, 0);
    return layout;
}
