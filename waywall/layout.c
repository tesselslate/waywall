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

static lua_State *current_vm;
static struct wall *current_wall;

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
create_instance_table(lua_State *L, struct wall *wall, int id) {
    struct state *state = &wall->instances[id].state;
    lua_newtable(L);

    lua_pushinteger(L, id);
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

    lua_pushboolean(L, wall->instance_data[id].locked);
    lua_setfield(L, -2, "locked");
}

static void
create_instances_table(lua_State *L, struct wall *wall) {
    lua_createtable(L, wall->instance_count, 0);

    for (size_t i = 0; i < wall->instance_count; i++) {
        create_instance_table(L, wall, i);
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
 *  Assumes the scene table is on the top of the stack.
 */
static bool
unmarshal_scene(lua_State *L, struct wall *wall, struct layout *layout) {
    size_t n = lua_objlen(L, -1);
    if (n == 0) {
        lua_settop(L, 0);
        lua_gc(L, LUA_GCCOLLECT, 0);
        return true;
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

static int
l_get_instance(lua_State *L) {
    ww_assert(current_wall);

    if (!lua_isnumber(L, -1)) {
        return luaL_error(L, "non-number instance ID given");
    }

    int id = lua_tointeger(L, -1);
    if (id < 0 || (size_t)id >= current_wall->instance_count) {
        return luaL_error(L, "instance ID out of bounds (%d)", id);
    }

    create_instance_table(L, current_wall, id);
    return 1;
}

static int
l_get_instances(lua_State *L) {
    ww_assert(current_wall);

    create_instances_table(L, current_wall);
    return 1;
}

static int
l_get_screen_size(lua_State *L) {
    ww_assert(current_wall);
    lua_pushinteger(L, current_wall->screen_width);
    lua_pushinteger(L, current_wall->screen_height);
    return 2;
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
        {"__c_waywall_log", l_waywall_log},
        {"get_instance", l_get_instance},
        {"get_instances", l_get_instances},
        {"get_screen_size", l_get_screen_size},
        {NULL, NULL},
    };
    lua_getglobal(L, "_G");
    luaL_register(L, "waywall", lib);
    lua_pop(L, 1);

    lua_pushstring(L, g_config->generator_name);
    lua_setglobal(L, "__c_waywall_generator_name");

    if (g_config->force_jit) {
        lua_pushboolean(L, true);
        lua_setglobal(L, "__c_waywall_force_jit");
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

static void
vm_hook(lua_State *L, lua_Debug *dbg) {
    luaL_error(L, "layout generator exceeded instruction limit");
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

struct instance_list
layout_get_locked(struct wall *wall) {
    struct instance_list list = {0};

    lua_getglobal(current_vm, "__generator_get_locked");
    if (!lua_isfunction(current_vm, -1)) {
        return list;
    }

    current_wall = wall;
    if (!g_config->force_jit) {
        lua_sethook(current_vm, vm_hook, LUA_MASKCOUNT, 1000000);
    }
    if (lua_pcall(current_vm, 0, 1, 0) != 0) {
        wlr_log(WLR_ERROR, "failed to request locked instance list: %s",
                lua_tostring(current_vm, -1));
        goto fail;
    }
    if (!g_config->force_jit) {
        lua_sethook(current_vm, NULL, 0, 0);
    }
    current_wall = NULL;

    if (!lua_istable(current_vm, -1)) {
        wlr_log(WLR_ERROR, "layout generator did not return a table of locked instances");
        goto fail;
    }

    lua_pushnil(current_vm);
    while (lua_next(current_vm, -2)) {
        if (!lua_isnumber(current_vm, -1)) {
            wlr_log(WLR_ERROR,
                    "layout generator returned non-number instance ID in list of locked instances");
            goto fail;
        }

        list.ids[list.id_count++] = lua_tointeger(current_vm, -1);
        lua_pop(current_vm, 1);
    }

    lua_settop(current_vm, 0);
    lua_gc(current_vm, LUA_GCCOLLECT, 0);

    return list;

fail:
    lua_close(current_vm);
    current_vm = NULL;
    return list;
}

struct instance_bitfield
layout_get_reset_all(struct wall *wall) {
    struct instance_bitfield bitfield = {0};

    lua_getglobal(current_vm, "__generator_get_reset_all");
    if (!lua_isfunction(current_vm, -1)) {
        return bitfield;
    }

    current_wall = wall;
    if (!g_config->force_jit) {
        lua_sethook(current_vm, vm_hook, LUA_MASKCOUNT, 1000000);
    }
    if (lua_pcall(current_vm, 0, 1, 0) != 0) {
        wlr_log(WLR_ERROR, "failed to request reset all list: %s", lua_tostring(current_vm, -1));
        goto fail;
    }
    if (!g_config->force_jit) {
        lua_sethook(current_vm, NULL, 0, 0);
    }
    current_wall = NULL;

    if (!lua_istable(current_vm, -1)) {
        wlr_log(WLR_ERROR,
                "layout generator did not return a table of instance IDs for the reset all list");
        goto fail;
    }

    lua_pushnil(current_vm);
    while (lua_next(current_vm, -2)) {
        if (!lua_isnumber(current_vm, -1)) {
            wlr_log(WLR_ERROR,
                    "layout generator returned non-number instance ID in reset all list");
            goto fail;
        }

        int i = lua_tointeger(current_vm, -1);
        ww_assert(i >= 0 && i < MAX_INSTANCES);
        bitfield.bits[i / 8] |= (1 << (i % 8));
        lua_pop(current_vm, 1);
    }

    lua_settop(current_vm, 0);
    lua_gc(current_vm, LUA_GCCOLLECT, 0);

    return bitfield;

fail:
    lua_close(current_vm);
    current_vm = NULL;
    return bitfield;
}

bool
layout_init(struct wall *wall, struct layout *layout) {
    current_vm = vm_create();
    if (!current_vm) {
        return false;
    }

    struct layout_reason reason = {.cause = REASON_INIT};
    return layout_request_new(wall, reason, layout);
}

bool
layout_reinit(struct wall *wall, struct layout *layout) {
    lua_State *new_vm = vm_create();
    if (!new_vm) {
        return false;
    }

    lua_close(current_vm);
    current_vm = new_vm;

    struct layout_reason reason = {.cause = REASON_INIT};
    return layout_request_new(wall, reason, layout);
}

bool
layout_request_new(struct wall *wall, struct layout_reason reason, struct layout *layout) {
    if (!current_vm) {
        return false;
    }

    const char *func_name = reason.cause == REASON_INIT            ? "__generator_init"
                            : reason.cause == REASON_INSTANCE_ADD  ? "__generator_instance_spawn"
                            : reason.cause == REASON_INSTANCE_DIE  ? "__generator_instance_die"
                            : reason.cause == REASON_PREVIEW_START ? "__generator_preview_start"
                            : reason.cause == REASON_LOCK          ? "__generator_lock"
                            : reason.cause == REASON_UNLOCK        ? "__generator_unlock"
                            : reason.cause == REASON_RESET         ? "__generator_reset"
                            : reason.cause == REASON_RESET_ALL     ? "__generator_reset_all"
                            : reason.cause == REASON_RESET_INGAME  ? "__generator_reset_ingame"
                            : reason.cause == REASON_RESIZE        ? "__generator_resize"
                                                                   : NULL;
    ww_assert(func_name);
    lua_getglobal(current_vm, func_name);
    if (!lua_isfunction(current_vm, -1)) {
        return false;
    }

    int nargs = 0;
    switch (reason.cause) {
    case REASON_INIT:
        if (g_config->generator_options) {
            marshal_table(current_vm, g_config->generator_options);
        } else {
            lua_newtable(current_vm);
        }
        create_instances_table(current_vm, wall);
        nargs = 2;
        break;
    case REASON_RESIZE:
        lua_pushinteger(current_vm, reason.data.screen_size[0]);
        lua_pushinteger(current_vm, reason.data.screen_size[1]);
        nargs = 2;
        break;
    case REASON_INSTANCE_ADD:
    case REASON_INSTANCE_DIE:
    case REASON_PREVIEW_START:
    case REASON_LOCK:
    case REASON_UNLOCK:
    case REASON_RESET:
    case REASON_RESET_INGAME:
        lua_pushinteger(current_vm, reason.data.instance_id);
        nargs = 1;
        break;
    case REASON_RESET_ALL:
        nargs = 0;
        break;
    }

    current_wall = wall;
    if (!g_config->force_jit) {
        lua_sethook(current_vm, vm_hook, LUA_MASKCOUNT, 1000000);
    }
    if (lua_pcall(current_vm, nargs, 1, 0) != 0) {
        wlr_log(WLR_ERROR, "failed to request layout: %s", lua_tostring(current_vm, -1));
        goto fail;
    }
    if (!g_config->force_jit) {
        lua_sethook(current_vm, NULL, 0, 0);
    }
    current_wall = NULL;

    bool new_layout = lua_istable(current_vm, -1);
    if (new_layout) {
        if (!unmarshal_scene(current_vm, wall, layout)) {
            goto fail;
        }
    }
    lua_settop(current_vm, 0);
    lua_gc(current_vm, LUA_GCCOLLECT, 0);

    return new_layout;

fail:
    lua_close(current_vm);
    current_vm = NULL;
    return false;
}
