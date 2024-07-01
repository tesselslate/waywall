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
    static const int IDX_ELEMENT = 3;
    static const int IDX_INT = 4;

    // LUA STACK:
    // - layout element
    // - key
    // - layout
    ww_assert(lua_gettop(L) == IDX_ELEMENT);

    lua_pushstring(L, key);
    lua_rawget(L, IDX_ELEMENT);

    if (lua_type(L, IDX_INT) == LUA_TNUMBER) {
        double x = lua_tonumber(L, IDX_INT);
        int32_t ix = (int32_t)x;
        if (ix != x) {
            ww_log(LOG_ERROR, "expected key '%s' of layout element to be an integer, got '%lf'",
                   key, x);
            goto fail;
        }
        *dst = ix;
    } else {
        ww_log(LOG_ERROR, "expected key '%s' of layout element to be of type 'number', got '%s'",
               key, luaL_typename(L, IDX_INT));
        goto fail;
    }

    lua_pop(L, 1);
    ww_assert(lua_gettop(L) == IDX_ELEMENT);

    return 0;

fail:
    lua_pop(L, 1);
    return 1;
}

static int
unmarshal_layout_element(struct config *cfg, struct wall *wall,
                         struct config_layout_element *elem) {
    static const int IDX_ELEMENT = 3;
    static const int IDX_FIELD = 4;

    // LUA STACK:
    // - layout element
    // - key
    // - layout
    ww_assert(lua_gettop(cfg->L) == IDX_ELEMENT);

    if (!lua_istable(cfg->L, IDX_ELEMENT)) {
        ww_log(LOG_ERROR, "expected to receive layout element of type 'table', got '%s'",
               luaL_typename(cfg->L, IDX_ELEMENT));
        return 1;
    }

    // Element type
    lua_pushinteger(cfg->L, 1);
    lua_rawget(cfg->L, -2);
    if (lua_type(cfg->L, IDX_FIELD) == LUA_TSTRING) {
        const char *type = lua_tostring(cfg->L, IDX_FIELD);
        if (strcmp(type, "instance") == 0) {
            elem->type = LAYOUT_ELEMENT_INSTANCE;
        } else if (strcmp(type, "rectangle") == 0) {
            elem->type = LAYOUT_ELEMENT_RECTANGLE;
        } else {
            ww_log(LOG_ERROR, "received layout element of unknown type '%s'", type);
            goto fail_pop;
        }
    } else {
        ww_log(LOG_ERROR, "expected key '1' of layout element to be of type 'string', got '%s'",
               luaL_typename(cfg->L, IDX_FIELD));
        goto fail_pop;
    }
    lua_pop(cfg->L, 1);

    // Type-specific data
    lua_pushinteger(cfg->L, 2);
    lua_rawget(cfg->L, -2);
    switch (elem->type) {
    case LAYOUT_ELEMENT_INSTANCE:
        if (lua_type(cfg->L, IDX_FIELD) == LUA_TNUMBER) {
            int id = lua_tointeger(cfg->L, IDX_FIELD);
            if (id < 1 || id > wall->num_instances) {
                ww_log(LOG_ERROR, "layout element contains invalid instance %d", id);
                goto fail_pop;
            }
            elem->data.instance = id - 1;
        } else {
            ww_log(LOG_ERROR,
                   "expected key '2' of instance layout element to be of type 'number', got '%s'",
                   luaL_typename(cfg->L, IDX_FIELD));
            goto fail_pop;
        }
        break;
    case LAYOUT_ELEMENT_RECTANGLE:
        if (lua_type(cfg->L, IDX_FIELD) == LUA_TSTRING) {
            const char *raw_rgba = lua_tostring(cfg->L, -1);
            if (config_parse_hex(elem->data.rectangle, raw_rgba) != 0) {
                ww_log(LOG_ERROR, "layout element contains invalid hex color '%s'", raw_rgba);
                goto fail_pop;
            }
        } else {
            ww_log(LOG_ERROR,
                   "expected key '2' of rectangle layout element to be of type 'string', got '%s'",
                   luaL_typename(cfg->L, IDX_FIELD));
            goto fail_pop;
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

fail_pop:
    lua_pop(cfg->L, 1);
    return 1;
}

static struct config_layout *
unmarshal_layout(struct config *cfg, struct wall *wall) {
    static const int IDX_LAYOUT = 1;
    static const int IDX_LAYOUT_KEY = 2;

    // LUA STACK:
    // - layout
    ww_assert(lua_gettop(cfg->L) == IDX_LAYOUT);

    if (!lua_istable(cfg->L, IDX_LAYOUT)) {
        ww_log(LOG_ERROR, "expected to receive layout of type 'table', got '%s'",
               luaL_typename(cfg->L, IDX_LAYOUT));
        lua_pop(cfg->L, 1);
        return NULL;
    }

    struct config_layout *layout = zalloc(1, sizeof(*layout));

    layout->num_elements = lua_objlen(cfg->L, IDX_LAYOUT);
    if (layout->num_elements == 0) {
        lua_pop(cfg->L, 1);
        return layout;
    }

    layout->elements = zalloc(layout->num_elements, sizeof(*layout->elements));

    size_t i = 0;
    lua_pushnil(cfg->L);
    while (lua_next(cfg->L, IDX_LAYOUT)) {
        // LUA STACK:
        // - layout element
        // - key
        // - layout
        struct config_layout_element *element = &layout->elements[i++];
        if (unmarshal_layout_element(cfg, wall, element) != 0) {
            goto fail_element;
        }

        // Pop the value from the top of the stack. The previous key will be left at the top of the
        // stack for the next call to `lua_next`.
        lua_pop(cfg->L, 1);
        ww_assert(lua_gettop(cfg->L) == IDX_LAYOUT_KEY);
    }

    // Pop the layout table.
    lua_pop(cfg->L, 1);
    ww_assert(lua_gettop(cfg->L) == 0);

    return layout;

fail_element:
    // Pop the layout element, key, and layout table.
    lua_pop(cfg->L, 3);
    ww_assert(lua_gettop(cfg->L) == 0);

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
    static const int IDX_LAYOUT = 1;

    ww_assert(lua_gettop(cfg->L) == 0);

    lua_pushlightuserdata(cfg->L, (void *)&config_registry_keys.layout);
    lua_rawget(cfg->L, LUA_REGISTRYINDEX);

    if (lua_isnil(cfg->L, IDX_LAYOUT)) {
        lua_pop(cfg->L, 1);
        ww_assert(lua_gettop(cfg->L) == 0);

        return NULL;
    }

    ww_assert(lua_istable(cfg->L, IDX_LAYOUT));
    struct config_layout *layout = unmarshal_layout(cfg, wall);

    ww_assert(lua_gettop(cfg->L) == 0);
    return layout;
}
