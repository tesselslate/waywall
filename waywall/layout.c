#include "layout.h"
#include "instance.h"
#include "wall.h"
#include "waywall.h"
#include <luajit-2.1/lauxlib.h>
#include <luajit-2.1/lua.h>
#include <luajit-2.1/lualib.h>
#include <stdlib.h>
#include <toml.h>
#include <wlr/util/log.h>

// TODO: Improve error messages (log broken objects?)
// TODO: Additional checks (e.g. no duplicate instances until that is supported, instance IDs
// inbounds)
// TODO: Provide utilities to lua (e.g. check for validity of hex colors, later on support things
// like cropping and percentage display)

#define GLOBAL_CONFIG "__c_waywall_config"
#define GLOBAL_FORCE_JIT "__c_waywall_force_jit"
#define GLOBAL_GENERATOR_NAME "__c_waywall_generator_name"
#define GLOBAL_LOG_FUNC "__c_waywall_log"
#define GLOBAL_REQUEST_FUNC "__waywall_request"

static lua_State *current_vm;

static bool marshal_array(lua_State *L, const toml_array_t *array);
static bool marshal_table(lua_State *L, const toml_table_t *table);

static bool
marshal_array(lua_State *L, const toml_array_t *array) {
    lua_newtable(L);

    int n = toml_array_nelem(array);
    for (int i = 0; i < n; i++) {
        toml_raw_t raw = toml_raw_at(array, i);

        if (raw) {
            char *s;
            if (toml_rtos(raw, &s) == 0) {
                lua_pushstring(L, s);
                lua_rawseti(L, -2, i + 1);
                free(s);
                continue;
            }

            int b;
            if (toml_rtob(raw, &b) == 0) {
                lua_pushboolean(L, b);
                lua_rawseti(L, -2, i + 1);
                continue;
            }

            int64_t t_i;
            if (toml_rtoi(raw, &t_i) == 0) {
                lua_pushinteger(L, (lua_Integer)t_i);
                lua_rawseti(L, -2, i + 1);
                continue;
            }

            double d;
            if (toml_rtod(raw, &d) == 0) {
                lua_pushnumber(L, (lua_Number)d);
                lua_rawseti(L, -2, i + 1);
                continue;
            }

            toml_timestamp_t ts;
            if (toml_rtots(raw, &ts) == 0) {
                wlr_log(WLR_ERROR, "timestamps are not supported");
                return false;
            }
        } else {
            toml_array_t *a = toml_array_at(array, i);
            if (a) {
                marshal_array(L, a);
                lua_rawseti(L, -2, i + 1);
                continue;
            }

            toml_table_t *t = toml_table_at(array, i);
            if (t) {
                marshal_table(L, t);
                lua_rawseti(L, -2, i + 1);
                continue;
            }
        }

        wlr_log(WLR_ERROR, "found unknown type when processing TOML");
        ww_unreachable();
    }

    return true;
}

static bool
marshal_table(lua_State *L, const toml_table_t *table) {
    lua_newtable(L);

    int n = toml_table_nkval(table) + toml_table_narr(table) + toml_table_ntab(table);
    for (int i = 0; i < n; i++) {
        const char *key = toml_key_in(table, i);
        ww_assert(key);
        toml_raw_t raw = toml_raw_in(table, key);

        if (raw) {
            char *s;
            if (toml_rtos(raw, &s) == 0) {
                lua_pushstring(L, s);
                lua_setfield(L, -2, key);
                free(s);
                continue;
            }

            int b;
            if (toml_rtob(raw, &b) == 0) {
                lua_pushboolean(L, b);
                lua_setfield(L, -2, key);
                continue;
            }

            int64_t t_i;
            if (toml_rtoi(raw, &t_i) == 0) {
                lua_pushinteger(L, (lua_Integer)t_i);
                lua_setfield(L, -2, key);
                continue;
            }

            double d;
            if (toml_rtod(raw, &d) == 0) {
                lua_pushnumber(L, (lua_Number)d);
                lua_setfield(L, -2, key);
                continue;
            }

            toml_timestamp_t ts;
            if (toml_rtots(raw, &ts) == 0) {
                wlr_log(WLR_ERROR, "timestamps are not supported");
                return false;
            }
        } else {
            toml_array_t *arr = toml_array_in(table, key);
            if (arr) {
                marshal_array(L, arr);
                lua_setfield(L, -2, key);
                continue;
            }

            toml_table_t *tab = toml_table_in(table, key);
            if (tab) {
                marshal_table(L, tab);
                lua_setfield(L, -2, key);
                continue;
            }
        }

        wlr_log(WLR_ERROR, "found unknown type when processing TOML");
        ww_unreachable();
    }

    return true;
}

static void
create_config_table(lua_State *L) {
    if (!g_config->generator_options) {
        lua_newtable(L);
        lua_setglobal(L, GLOBAL_CONFIG);
    } else {
        marshal_table(L, g_config->generator_options);
        lua_setglobal(L, GLOBAL_CONFIG);
    }
}

static void
create_state_table(lua_State *L, struct wall *wall) {
    lua_createtable(L, wall->instance_count, 0);

    for (size_t i = 0; i < wall->instance_count; i++) {
        struct state *state = &wall->instances[i].state;
        lua_newtable(L);

        lua_pushinteger(L, i);
        lua_setfield(L, -2, "id");

        char *state_name;
        switch (state->screen) {
        case TITLE:
            state_name = "title";
            break;
        case GENERATING:
            state_name = "generating";
            break;
        case WAITING:
            state_name = "waiting";
            break;
        case PREVIEWING:
            state_name = "previewing";
            break;
        case INWORLD:
            state_name = "inworld";
            break;
        default:
            ww_unreachable();
        }
        lua_pushstring(L, state_name);
        lua_setfield(L, -2, "screen");

        if (state->screen == GENERATING || state->screen == PREVIEWING) {
            lua_pushinteger(L, state->data.percent);
            lua_setfield(L, -2, "gen_percent");
        }

        if (state->screen == PREVIEWING) {
            struct timespec last_preview = wall->instances[i].last_preview;
            uint64_t ms = last_preview.tv_sec * 1000 + last_preview.tv_nsec / 1000000;
            lua_pushinteger(L, ms);
            lua_setfield(L, -2, "preview_start");
        }

        lua_pushboolean(L, wall->instance_data[i].locked);
        lua_setfield(L, -2, "locked");

        lua_rawseti(L, -2, i + 1);
    }
}

/*
 *  Assumes the layout entry table is on the top of the stack.
 */
static bool
unmarshal_layout_entry(lua_State *L, struct layout_entry *entry) {
    // Perform basic checks.
    if (!lua_istable(L, -1)) {
        wlr_log(WLR_ERROR, "found non-table entry");
        return false;
    }

    lua_pushnil(L);
    if (!lua_next(L, -2)) {
        wlr_log(WLR_ERROR, "layout generator returned an empty entry");
        return false;
    }
    if (!lua_isstring(L, -1)) {
        wlr_log(WLR_ERROR, "layout generator returned an entry that did not begin with a string");
        return false;
    }

    // Check the entry type.
    const char *type = lua_tostring(L, -1);
    if (strcmp(type, "instance") == 0) {
        entry->type = INSTANCE;
        lua_pop(L, 1);
        if (!lua_next(L, -2)) {
            wlr_log(WLR_ERROR, "layout generator did not return an instance ID");
            return false;
        }
        if (!lua_isnumber(L, -1)) {
            wlr_log(WLR_ERROR, "layout generator returned non-number instance ID");
        }
        entry->data.instance = lua_tointeger(L, -1);
        lua_pop(L, 1);
    } else if (strcmp(type, "rectangle") == 0) {
        entry->type = RECTANGLE;
        lua_pop(L, 1);
    } else {
        wlr_log(WLR_ERROR, "received unknown entry type from layout generator ('%s')", type);
        return false;
    }

    // Look at the various keys.
    bool x, y, w, h, color;
    x = y = w = h = color = false;
    while (lua_next(L, -2)) {
        // There should be no more unkeyed values.
        if (!lua_isstring(L, -2)) {
            wlr_log(WLR_ERROR,
                    "layout generator returned an invalid entry (too many unkeyed values)");
            return false;
        }

        bool is_number = lua_isnumber(L, -1);
        const char *key = lua_tostring(L, -2);
        if (strcmp(key, "x") == 0) {
            if (!is_number) {
                wlr_log(WLR_ERROR, "layout generator returned non-number X coordinate");
                return false;
            }
            entry->x = lua_tointeger(L, -1);
            x = true;
        } else if (strcmp(key, "y") == 0) {
            if (!is_number) {
                wlr_log(WLR_ERROR, "layout generator returned non-number Y coordinate");
                return false;
            }
            entry->y = lua_tointeger(L, -1);
            y = true;
        } else if (strcmp(key, "w") == 0) {
            if (!is_number) {
                wlr_log(WLR_ERROR, "layout generated returned non-number width");
                return false;
            }
            entry->w = lua_tointeger(L, -1);
            w = true;
        } else if (strcmp(key, "h") == 0) {
            if (!is_number) {
                wlr_log(WLR_ERROR, "layout generated returned non-number width");
                return false;
            }
            entry->h = lua_tointeger(L, -1);
            h = true;
        } else if (strcmp(key, "color") == 0) {
            if (entry->type != RECTANGLE) {
                wlr_log(WLR_ERROR, "layout generator returned color for an instance");
                return false;
            }
            if (!lua_isstring(L, -1)) {
                wlr_log(WLR_ERROR, "layout generator returned non-string color");
                return false;
            }
            if (!ww_util_parse_color(entry->data.color, lua_tostring(L, -1))) {
                wlr_log(WLR_ERROR, "layout generator returned invalid color ('%s')",
                        lua_tostring(L, -1));
                return false;
            }
            color = true;
        } else {
            wlr_log(WLR_ERROR, "extraneous key-value pair in layout entry ('%s')", key);
            return false;
        }
        lua_pop(L, 1);
    }

    // Check that the necessary keys were present.
    if (!x || !y || !w || !h) {
        wlr_log(WLR_ERROR, "layout entry missing dimensions");
        return false;
    }
    if (!color && entry->type == RECTANGLE) {
        wlr_log(WLR_ERROR, "layout rectangle missing color");
        return false;
    }

    return true;
}

/*
 *  Assumes the reset all table is on the top of the stack.
 */
static bool
unmarshal_reset_all_list(lua_State *L, struct wall *wall, struct layout *layout) {
    size_t n = lua_objlen(L, -1);
    if (n == 0) {
        return true;
    }

    int i = 0;
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        if (lua_isnumber(L, -2)) {
            int j = lua_tointeger(L, -2);
            if (j != i + 1) {
                wlr_log(
                    WLR_ERROR,
                    "layout generator did not return a sequential array of instance IDs (%d -> %d)",
                    i, j);
                return false;
            }
            i = j;

            if (!lua_isnumber(L, -1)) {
                wlr_log(WLR_ERROR,
                        "layout generator returned a non-number instance ID in the reset all list");
                return false;
            }
            int id = lua_tointeger(L, -1);
            if (id < 0 || (size_t)id > wall->instance_count) {
                wlr_log(WLR_ERROR,
                        "layout generator returned an invalid instance ID in the reset all list");
                return false;
            }
            layout->reset_all_ids[id / 64] |= (1 << (id % 64));
        } else {
            wlr_log(WLR_ERROR, "layout generator did not return an array of instance IDs");
            return false;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return true;
}

/*
 *  Assumes the scene table is on the top of the stack.
 */
static bool
unmarshal_scene(lua_State *L, struct wall *wall, struct layout *layout) {
    size_t n = lua_objlen(L, -1);
    if (n == 0) {
        lua_settop(L, 0);
        lua_gc(L, LUA_GCCOLLECT, 0);
        return layout;
    }
    layout->entry_count = n;
    layout->entries = calloc(n, sizeof(struct layout_entry));

    int i = 0;
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        if (lua_isnumber(L, -2)) {
            int j = lua_tointeger(L, -2);
            if (j != i + 1) {
                wlr_log(WLR_ERROR,
                        "layout generator did not return a sequential array of objects (%d -> %d)",
                        i, j);
                return false;
            }
            i = j;
        } else {
            wlr_log(WLR_ERROR, "layout generator did not return an array of objects");
            return false;
        }

        // The top of the stack (should) now contain a table representing an individual entry.
        // { "type", ... }
        struct layout_entry *entry = &layout->entries[i - 1];
        if (!unmarshal_layout_entry(L, entry)) {
            return false;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return true;
}

static int
l_waywall_log(lua_State *L) {
    wlr_log(WLR_INFO, "layout: %s", lua_tostring(L, 1));
    return 0;
}

static lua_State *
vm_create() {
    lua_State *L = luaL_newstate();
    if (!L) {
        wlr_log(WLR_ERROR, "failed to create new lua state");
        return NULL;
    }

    luaL_openlibs(L);
    static const struct luaL_Reg lib[] = {
        {GLOBAL_LOG_FUNC, l_waywall_log},
        {NULL, NULL},
    };
    lua_getglobal(L, "_G");
    luaL_register(L, NULL, lib);
    lua_pop(L, 1);

    lua_pushstring(L, g_config->generator_name);
    lua_setglobal(L, GLOBAL_GENERATOR_NAME);
    create_config_table(L);

    if (g_config->force_jit) {
        lua_pushboolean(L, true);
        lua_setglobal(L, GLOBAL_FORCE_JIT);
    }

    if (luaL_dostring(L, "require('boot')")) {
        wlr_log(WLR_ERROR, "failed to create layout VM: %s", lua_tostring(L, -1));
        luaL_traceback(L, L, NULL, 0);
        lua_pop(L, 1);
        lua_close(L);
        return NULL;
    }
    return L;
}

void
layout_destroy(struct layout layout) {
    if (layout.entries) {
        free(layout.entries);
    }
}

void
layout_fini() {
    if (current_vm) {
        lua_close(current_vm);
    }
}

bool
layout_init() {
    current_vm = vm_create();
    if (!current_vm) {
        return false;
    }

    return true;
}

bool
layout_reinit() {
    lua_State *new_vm = vm_create();
    if (!new_vm) {
        return false;
    }

    lua_close(current_vm);
    current_vm = new_vm;
    return true;
}

struct layout
layout_request_new(struct wall *wall) {
    struct layout layout = {0};
    if (!current_vm) {
        return layout;
    }

    lua_getglobal(current_vm, GLOBAL_REQUEST_FUNC);
    create_state_table(current_vm, wall);
    lua_pushinteger(current_vm, wall->screen_width);
    lua_pushinteger(current_vm, wall->screen_height);
    if (lua_pcall(current_vm, 3, 2, 0) != 0) {
        wlr_log(WLR_ERROR, "failed to request layout: %s", lua_tostring(current_vm, -1));
        goto fail;
    }

    if (!unmarshal_reset_all_list(current_vm, wall, &layout)) {
        goto fail;
    }

    if (!unmarshal_scene(current_vm, wall, &layout)) {
        goto fail;
    }
    lua_settop(current_vm, 0);
    lua_gc(current_vm, LUA_GCCOLLECT, 0);

    return layout;

fail:
    lua_close(current_vm);
    current_vm = NULL;
    return (struct layout){0};
}
