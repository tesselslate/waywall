#include "lua/api.h"
#include "lua/helpers.h"

#include "config/config.h"
#include "config/internal.h"
#include "config/vm.h"
#include "instance.h"
#include "server/server.h"
#include "server/ui.h"
#include "server/wl_seat.h"
#include "server/wp_relative_pointer.h"
#include "timer.h"
#include "util/alloc.h"
#include "util/box.h"
#include "util/keycodes.h"
#include "util/log.h"
#include "util/prelude.h"
#include "wrap.h"
#include <luajit-2.1/lauxlib.h>
#include <luajit-2.1/lua.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <time.h>
#include <xkbcommon/xkbcommon.h>

#ifdef WAYWALL_OPENGL

#include "server/gl.h"

#endif

/*
 * Lua interop code can be a bit obtuse due to working with the stack. The code in this file follows
 * a few conventions:
 *
 *  1. Each Lua API function should be split into 3 sections, each labeled with a comment:
 *
 *       a. Prologue: retrieve and validate arguments, ensure stack ends with last argument
 *       b. Body: perform the actual operation
 *       c. Epilogue: push return values to the stack and end the function
 *
 *     Some notes:
 *
 *       - Return values may be pushed to the stack during the body, but this should be noted in the
 *         epilogue comment.
 *       - If the prologue and/or body are not present, their comments can be omitted.
 *       - If there are any number of arguments, lua_settop() should be called to ensure the stack
 *         size is correct, even if the stack is not used later in the function. This ensures that
 *         the check will be present if the function is later modified to make use of the stack.
 *
 *  2. Calls to lua_* functions which modify the stack should be postfixed with a comment stating
 *     the current stack top.
 *
 *        - In some cases, the stack top is irrelevant or obvious (i.e. after calls to lua_settop or
 *          when pushing arguments at the end of a function). When this happens, there's no need to
 *          write a comment noting the stack top.
 *
 *  3. Constant stack indices should be used wherever possible and labelled with an associated
 *     constant value at the start of the function (ARG_*, IDX_*).
 *
 * You should also attempt to follow some of these conventions (stack top comments, constant stack
 * indices) in the Lua interop code present in other files.
 */

static const struct {
    const unsigned char *data;
    size_t size;
    const char *name;
} EMBEDDED_LUA[] = {
    {luaJIT_BC_api, luaJIT_BC_api_SIZE, "waywall"},
    {luaJIT_BC_helpers, luaJIT_BC_helpers_SIZE, "waywall.helpers"},
};

#define METATABLE_MIRROR "waywall.mirror"

#define STARTUP_ERRMSG(function) function " cannot be called during startup"

struct waker_sleep {
    struct ww_timer_entry *timer;
    struct config_vm_waker *vm;
};

#ifdef WAYWALL_OPENGL

static int
mirror_close(lua_State *L) {
    struct server_gl_mirror **mirror = lua_touserdata(L, 1);

    if (!*mirror) {
        return luaL_error(L, "cannot close mirror more than once");
    }

    server_gl_mirror_destroy(*mirror);
    *mirror = NULL;

    return 0;
}

static int
mirror_index(lua_State *L) {
    const char *key = luaL_checkstring(L, 2);

    if (strcmp(key, "close") == 0) {
        lua_pushcfunction(L, mirror_close);
    } else {
        lua_pushnil(L);
    }

    return 1;
}

static int
mirror_gc(lua_State *L) {
    struct server_gl_mirror **mirror = lua_touserdata(L, 1);

    if (*mirror) {
        server_gl_mirror_destroy(*mirror);
    }
    *mirror = NULL;

    return 0;
}

#endif

static void
waker_sleep_vm_destroy(struct config_vm_waker *vm_waker, void *data) {
    struct waker_sleep *waker = data;

    if (waker->timer) {
        ww_timer_entry_destroy(waker->timer);
    }

    free(waker);
}

static void
waker_sleep_timer_destroy(void *data) {
    struct waker_sleep *waker = data;

    // This function is called if the timer entry is destroyed (which should only happen if the
    // global timer manager is destroyed.)
    //
    // Remove the reference to the timer entry so that when the VM attempts to destroy the waker we
    // do not attempt to destroy the timer entry a 2nd time.
    waker->timer = NULL;
}

static void
waker_sleep_timer_fire(void *data) {
    struct waker_sleep *waker = data;

    config_vm_resume(waker->vm);
}

WW_MAYBE_UNUSED static int
unmarshal_box(lua_State *L, const char *key, struct box *out) {
    lua_pushstring(L, key); // stack: n+1
    lua_rawget(L, -2);      // stack: n+1

    if (lua_type(L, -1) != LUA_TTABLE) {
        return luaL_error(L, "expected '%s' to be a table, got '%s'", key, luaL_typename(L, -1));
    }

    const struct {
        const char *key;
        int32_t *out;
    } pairs[] = {
        {"x", &out->x},
        {"y", &out->y},
        {"w", &out->width},
        {"h", &out->height},
    };

    for (size_t i = 0; i < STATIC_ARRLEN(pairs); i++) {
        lua_pushstring(L, pairs[i].key); // stack: n+2
        lua_rawget(L, -2);               // stack: n+2

        if (lua_type(L, -1) != LUA_TNUMBER) {
            return luaL_error(L, "expected '%s.%s' to be a number, got '%s'", key, pairs[i].key,
                              luaL_typename(L, -1));
        }

        int x = lua_tointeger(L, -1);
        if (x < 0) {
            return luaL_error(L, "expected '%s.%s' to be positive", key, pairs[i].key);
        }

        *pairs[i].out = x;
        lua_pop(L, 1); // stack: n+1
    }

    lua_pop(L, 1); // stack: n

    return 0;
}

WW_MAYBE_UNUSED static int
unmarshal_color(lua_State *L, const char *key, float rgba[static 4]) {
    lua_pushstring(L, key); // stack: n+1
    lua_rawget(L, -2);      // stack: n+1

    if (lua_type(L, -1) != LUA_TSTRING) {
        return luaL_error(L, "expected '%s' to be a string, got '%s'", key, luaL_typename(L, -1));
    }

    const char *value = lua_tostring(L, -1);

    uint8_t u8_rgba[4] = {0};
    if (config_parse_hex(u8_rgba, value) != 0) {
        return luaL_error(L, "expected '%s' to be a valid hex color ('%s')", key, value);
    }

    rgba[0] = (float)u8_rgba[0] / UINT8_MAX;
    rgba[1] = (float)u8_rgba[1] / UINT8_MAX;
    rgba[2] = (float)u8_rgba[2] / UINT8_MAX;
    rgba[3] = (float)u8_rgba[3] / UINT8_MAX;

    lua_pop(L, 1); // stack: n

    return 0;
}

static int
l_active_res(lua_State *L) {
    // Prologue
    struct config_vm *vm = config_vm_from(L);
    struct wrap *wrap = config_vm_get_wrap(vm);
    if (!wrap) {
        return luaL_error(L, STARTUP_ERRMSG("active_res"));
    }

    // Epilogue
    lua_pushinteger(L, wrap->active_res.w);
    lua_pushinteger(L, wrap->active_res.h);
    return 2;
}

static int
l_current_time(lua_State *L) {
    // Body
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint32_t time = (uint32_t)((uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000);

    // Epilogue
    lua_pushinteger(L, time);
    return 1;
}

static int
l_exec(lua_State *L) {
    static const int ARG_COMMAND = 1;

    // Prologue
    struct config_vm *vm = config_vm_from(L);
    struct wrap *wrap = config_vm_get_wrap(vm);
    if (!wrap) {
        return luaL_error(L, STARTUP_ERRMSG("exec"));
    }

    const char *lua_str = luaL_checkstring(L, ARG_COMMAND);

    lua_settop(L, ARG_COMMAND);

    // Body. Duplicate the string from the Lua VM so that it can be modified for in-place argument
    // parsing.
    char *cmd_str = strdup(lua_str);
    check_alloc(cmd_str);

    char *cmd[64] = {0};
    char *needle = cmd_str;
    char *elem;

    bool ok = true;
    size_t i = 0;
    while (ok) {
        elem = needle;
        while (*needle && *needle != ' ') {
            needle++;
        }
        ok = !!*needle;
        *needle = '\0';
        needle++;

        cmd[i++] = elem;
        if (ok && i == STATIC_ARRLEN(cmd)) {
            free(cmd_str);
            return luaL_error(L, "command '%s' contains more than 63 arguments", lua_str);
        }
    }

    wrap_lua_exec(wrap, cmd);
    free(cmd_str);

    // Epilogue
    return 0;
}

static int
l_floating_shown(lua_State *L) {
    // Prologue
    struct config_vm *vm = config_vm_from(L);
    struct wrap *wrap = config_vm_get_wrap(vm);
    if (!wrap) {
        return luaL_error(L, STARTUP_ERRMSG("floating_shown"));
    }

    // Epilogue
    lua_pushboolean(L, wrap->floating.visible);
    return 1;
}

#ifdef WAYWALL_OPENGL

static int
l_mirror(lua_State *L) {
    static const int ARG_OPTIONS = 1;

    // Prologue
    struct config_vm *vm = config_vm_from(L);
    struct wrap *wrap = config_vm_get_wrap(vm);
    if (!wrap) {
        return luaL_error(L, STARTUP_ERRMSG("mirror"));
    }

    luaL_checktype(L, ARG_OPTIONS, LUA_TTABLE);
    lua_settop(L, ARG_OPTIONS);

    struct server_gl_mirror_options options = {0};

    unmarshal_box(L, "src", &options.src);
    unmarshal_box(L, "dst", &options.dst);

    lua_pushstring(L, "color_key"); // stack: 2
    lua_rawget(L, ARG_OPTIONS);     // stack: 2

    if (lua_type(L, -1) == LUA_TTABLE) {
        unmarshal_color(L, "input", options.src_rgba);
        unmarshal_color(L, "output", options.dst_rgba);
    }
    lua_pop(L, 1); // stack: 1

    // Body
    struct server_gl_mirror **mirror = lua_newuserdata(L, sizeof(*mirror));
    check_alloc(mirror);

    luaL_getmetatable(L, METATABLE_MIRROR);
    lua_setmetatable(L, -2);

    *mirror = server_gl_mirror_create(wrap->gl, options);
    if (!*mirror) {
        return luaL_error(L, "failed to create mirror");
    }

    // Epilogue. The userdata (mirror) was already pushed to the stack by the above code.
    return 1;
}

#else

static int
l_mirror(lua_State *L) {
    return luaL_error(L, "OpenGL support is not enabled");
}

#endif

static int
l_press_key(lua_State *L) {
    static const int ARG_KEYNAME = 1;

    // Prologue
    struct config_vm *vm = config_vm_from(L);
    struct wrap *wrap = config_vm_get_wrap(vm);
    if (!wrap) {
        return luaL_error(L, STARTUP_ERRMSG("press_key"));
    }

    const char *key = luaL_checkstring(L, ARG_KEYNAME);

    lua_settop(L, ARG_KEYNAME);

    // Body. Determine which keycode to send to the Minecraft instance.
    uint32_t keycode = KEY_UNKNOWN;
    for (size_t i = 0; i < STATIC_ARRLEN(util_keycodes); i++) {
        if (strcasecmp(util_keycodes[i].name, key) == 0) {
            keycode = util_keycodes[i].value;
            break;
        }
    }
    if (keycode == KEY_UNKNOWN) {
        return luaL_error(L, "unknown key %s", key);
    }

    wrap_lua_press_key(wrap, keycode);

    // Epilogue
    return 0;
}

static int
l_profile(lua_State *L) {
    // Prologue
    struct config_vm *vm = config_vm_from(L);
    lua_settop(L, 0);

    // Epilogue
    if (vm->profile) {
        lua_pushstring(L, vm->profile);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int
l_set_keymap(lua_State *L) {
    static const int ARG_KEYMAP = 1;
    static const int IDX_VALUE = 2;

    // Prologue
    struct config_vm *vm = config_vm_from(L);
    struct wrap *wrap = config_vm_get_wrap(vm);
    if (!wrap) {
        return luaL_error(L, STARTUP_ERRMSG("set_keymap"));
    }

    luaL_argcheck(L, lua_istable(L, ARG_KEYMAP), ARG_KEYMAP, "expected table");

    lua_settop(L, ARG_KEYMAP);

    // Body. Construct an instance of xkb_rule_names from the provided options table.
    struct xkb_rule_names rule_names = {0};

    const struct {
        const char *key;
        const char **value;
    } mappings[] = {
        {"layout", &rule_names.layout},   {"model", &rule_names.model},
        {"rules", &rule_names.rules},     {"variant", &rule_names.variant},
        {"options", &rule_names.options},
    };

    for (size_t i = 0; i < STATIC_ARRLEN(mappings); i++) {
        lua_pushstring(L, mappings[i].key); // stack: ARG_KEYMAP + 1
        lua_rawget(L, ARG_KEYMAP);          // stack: ARG_KEYMAP + 1 (IDX_VALUE)

        switch (lua_type(L, IDX_VALUE)) {
        case LUA_TSTRING: {
            const char *value_str = lua_tostring(L, IDX_VALUE);
            *mappings[i].value = value_str;
            break;
        }
        case LUA_TNIL:
            break;
        default:
            return luaL_error(L, "expected '%s' to be of type 'string' or 'nil', was '%s'",
                              mappings[i].key, luaL_typename(L, IDX_VALUE));
        }

        lua_pop(L, 1); // stack: ARG_KEYMAP
    }

    server_seat_lua_set_keymap(wrap->server->seat, &rule_names);

    // Epilogue
    return 0;
}

static int
l_set_resolution(lua_State *L) {
    static const int ARG_WIDTH = 1;
    static const int ARG_HEIGHT = 2;

    // Prologue
    struct config_vm *vm = config_vm_from(L);
    struct wrap *wrap = config_vm_get_wrap(vm);
    if (!wrap) {
        return luaL_error(L, STARTUP_ERRMSG("set_resolution"));
    }

    int32_t width = luaL_checkint(L, ARG_WIDTH);
    int32_t height = luaL_checkint(L, ARG_HEIGHT);

    luaL_argcheck(L, width >= 0, ARG_WIDTH, "width must be non-negative");
    luaL_argcheck(L, height >= 0, ARG_HEIGHT, "height must be non-negative");

    lua_settop(L, ARG_HEIGHT);

    // Body
    bool ok = wrap_lua_set_res(wrap, width, height) == 0;
    if (!ok) {
        return luaL_error(L, "cannot set resolution");
    }

    // Epilogue
    return 0;
}

static int
l_set_sensitivity(lua_State *L) {
    static const int ARG_SENS = 1;

    // Prologue
    struct config_vm *vm = config_vm_from(L);
    struct wrap *wrap = config_vm_get_wrap(vm);
    if (!wrap) {
        return luaL_error(L, STARTUP_ERRMSG("set_sensitivity"));
    }

    double sens = luaL_checknumber(L, ARG_SENS);
    luaL_argcheck(L, sens >= 0, ARG_SENS, "sensitivity must be a positive number");

    lua_settop(L, ARG_SENS);

    // Body
    if (sens == 0) {
        sens = wrap->cfg->input.sens;
    }
    server_relative_pointer_set_sens(wrap->server->relative_pointer, sens);

    // Epilogue
    return 0;
}

static int
l_show_floating(lua_State *L) {
    static const int ARG_SHOW = 1;

    // Prologue
    struct config_vm *vm = config_vm_from(L);
    struct wrap *wrap = config_vm_get_wrap(vm);
    if (!wrap) {
        return luaL_error(L, STARTUP_ERRMSG("show_floating"));
    }

    luaL_argcheck(L, lua_type(L, ARG_SHOW) == LUA_TBOOLEAN, ARG_SHOW,
                  "visibility must be a boolean");
    bool show = lua_toboolean(L, ARG_SHOW);

    lua_settop(L, ARG_SHOW);

    // Body
    wrap_lua_show_floating(wrap, show);

    // Epilogue
    return 0;
}

static int
l_sleep(lua_State *L) {
    static const int ARG_MS = 1;

    // Prologue
    struct config_vm *vm = config_vm_from(L);
    struct wrap *wrap = config_vm_get_wrap(vm);
    if (!wrap) {
        return luaL_error(L, STARTUP_ERRMSG("sleep"));
    }

    if (!config_vm_is_thread(L)) {
        // This function can only be called from within a coroutine (i.e. a keybind handler.)
        return luaL_error(L, "sleep called from invalid execution context");
    }

    int ms = luaL_checkinteger(L, ARG_MS);

    lua_settop(L, ARG_MS);

    // Body. Setup the timer for this sleep call.
    struct timespec duration = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000,
    };

    struct waker_sleep *waker = zalloc(1, sizeof(*waker));
    waker->timer = ww_timer_add_entry(wrap->timer, duration, waker_sleep_timer_fire,
                                      waker_sleep_timer_destroy, waker);
    if (!waker->timer) {
        free(waker);
        return luaL_error(L, "failed to prepare sleep");
    }

    waker->vm = config_vm_create_waker(L, waker_sleep_vm_destroy, waker);
    ww_assert(waker);

    // Epilogue
    return lua_yield(L, 0);
}

static int
l_state(lua_State *L) {
    static const int IDX_STATE = 1;

    // Prologue
    struct config_vm *vm = config_vm_from(L);
    struct wrap *wrap = config_vm_get_wrap(vm);
    if (!wrap) {
        return luaL_error(L, STARTUP_ERRMSG("state"));
    }

    lua_settop(L, 0);

    // Body
    if (!wrap->instance) {
        return luaL_error(L, "no state output");
    }

    static const char *screen_names[] = {
        [SCREEN_TITLE] = "title",           [SCREEN_WAITING] = "waiting",
        [SCREEN_GENERATING] = "generating", [SCREEN_PREVIEWING] = "previewing",
        [SCREEN_INWORLD] = "inworld",       [SCREEN_WALL] = "wall",
    };

    static const char *inworld_names[] = {
        [INWORLD_UNPAUSED] = "unpaused",
        [INWORLD_PAUSED] = "paused",
        [INWORLD_MENU] = "menu",
    };

    struct instance_state *state = &wrap->instance->state;

    lua_newtable(L); // stack: IDX_STATE

    lua_pushstring(L, "screen");                    // stack: IDX_STATE + 1 (key)
    lua_pushstring(L, screen_names[state->screen]); // stack: IDX_STATE + 2 (value)
    lua_rawset(L, IDX_STATE);                       // stack: IDX_STATE

    if (state->screen == SCREEN_GENERATING || state->screen == SCREEN_PREVIEWING) {
        lua_pushstring(L, "percent");            // stack: IDX_STATE + 1 (key)
        lua_pushinteger(L, state->data.percent); // stack: IDX_STATE + 2 (value)
        lua_rawset(L, IDX_STATE);                // stack: IDX_STATE
    } else if (state->screen == SCREEN_INWORLD) {
        lua_pushstring(L, "inworld");                          // stack: IDX_STATE + 1 (key)
        lua_pushstring(L, inworld_names[state->data.inworld]); // stack: IDX_STATE + 2 (value)
        lua_rawset(L, IDX_STATE);                              // stack: IDX_STATE
    }

    // Epilogue. The state table was already pushed to the stack by the above code.
    ww_assert(lua_gettop(L) == IDX_STATE);
    return 1;
}

static int
l_log(lua_State *L) {
    ww_log(LOG_INFO, "lua: %s", lua_tostring(L, 1));
    return 0;
}

static int
l_log_error(lua_State *L) {
    ww_log(LOG_ERROR, "lua: %s", lua_tostring(L, 1));
    return 0;
}

static int
l_register(lua_State *L) {
    static const int ARG_SIGNAL = 1;
    static const int ARG_HANDLER = 2;

    // Prologue
    struct config_vm *vm = config_vm_from(L);

    const char *signal = luaL_checkstring(L, ARG_SIGNAL);
    luaL_argcheck(L, lua_type(L, ARG_HANDLER) == LUA_TFUNCTION, ARG_HANDLER,
                  "handler must be a function");

    lua_settop(L, ARG_HANDLER);

    // Body
    config_vm_register_event(vm, L, signal);

    // Epilogue
    return 0;
}

static int
l_setenv(lua_State *L) {
    static const int ARG_NAME = 1;
    static const int ARG_VALUE = 2;

    // Prologue
    const char *name = luaL_checkstring(L, ARG_NAME);
    const char *value = NULL;
    switch (lua_type(L, ARG_VALUE)) {
    case LUA_TSTRING:
        value = lua_tostring(L, ARG_VALUE);
        break;
    case LUA_TNIL:
        break;
    default:
        return luaL_error(L, "expected value to be of type 'string' or 'nil', was '%s'",
                          luaL_typename(L, ARG_VALUE));
    }

    lua_settop(L, ARG_VALUE);

    // Body
    if (value) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }

    // Epilogue
    return 0;
}

static const struct luaL_Reg lua_lib[] = {
    // public (see api.lua)
    {"active_res", l_active_res},
    {"current_time", l_current_time},
    {"exec", l_exec},
    {"floating_shown", l_floating_shown},
    {"mirror", l_mirror},
    {"press_key", l_press_key},
    {"profile", l_profile},
    {"set_keymap", l_set_keymap},
    {"set_resolution", l_set_resolution},
    {"set_sensitivity", l_set_sensitivity},
    {"show_floating", l_show_floating},
    {"sleep", l_sleep},
    {"state", l_state},

    // private (see init.lua)
    {"log", l_log},
    {"log_error", l_log_error},
    {"register", l_register},
    {"setenv", l_setenv},
    {NULL, NULL},
};

int
config_api_init(struct config_vm *vm) {
    config_vm_register_lib(vm, lua_lib, "priv_waywall");

#ifdef WAYWALL_OPENGL

    // Create the metatable for "mirror" objects.
    luaL_newmetatable(vm->L, METATABLE_MIRROR); // stack: n+1
    lua_pushstring(vm->L, "__gc");              // stack: n+2
    lua_pushcfunction(vm->L, mirror_gc);        // stack: n+3
    lua_settable(vm->L, -3);                    // stack: n+1
    lua_pushstring(vm->L, "__index");           // stack: n+2
    lua_pushcfunction(vm->L, mirror_index);     // stack: n+3
    lua_settable(vm->L, -3);                    // stack: n+1
    lua_pop(vm->L, 1);                          // stack: n

#endif

    for (size_t i = 0; i < STATIC_ARRLEN(EMBEDDED_LUA); i++) {
        if (config_vm_exec_bcode(vm, EMBEDDED_LUA[i].data, EMBEDDED_LUA[i].size,
                                 EMBEDDED_LUA[i].name) != 0) {
            return 1;
        }
    }

    return 0;
}
