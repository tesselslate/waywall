#include "config/vm.h"
#include "config/internal.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include <luajit-2.1/lauxlib.h>
#include <luajit-2.1/lua.h>
#include <luajit-2.1/luajit.h>
#include <luajit-2.1/lualib.h>
#include <wayland-util.h>

struct config_vm_waker {
    struct wl_list link; // config_vm.wakers

    struct lua_State *L;

    void *data;
    config_vm_waker_destroy_func_t destroy;
};

static const struct {
    char actions;
    char coroutines;
    char events;

    char config_vm;
    char wrap;
} REG_KEYS = {0};

#define MAX_INSTRUCTIONS 50000000

static void waker_destroy(struct config_vm_waker *waker);
static struct config_vm_waker *waker_lookup(lua_State *L);

static void
on_debug_hook(lua_State *L, struct lua_Debug *dbg) {
    luaL_error(L, "instruction count exceeded");
}

static int
on_lua_panic(lua_State *L) {
    ww_log(LOG_ERROR, "LUA PANIC: %s", lua_tostring(L, -1));
    return 0;
}

static void
coro_table_add(lua_State *L, struct config_vm_waker *waker) {
    ssize_t stack_start = lua_gettop(L);

    lua_pushlightuserdata(L, (void *)&REG_KEYS.coroutines); // stack: n+1
    lua_rawget(L, LUA_REGISTRYINDEX);                       // stack: n+1
    lua_pushthread(L);                                      // stack: n+2
    lua_pushlightuserdata(L, (void *)waker);                // stack: n+3
    lua_rawset(L, -3);                                      // stack: n+1

    lua_pop(L, 1); // stack: n
    ww_assert(lua_gettop(L) == stack_start);
}

static void
coro_table_del(lua_State *L) {
    ssize_t stack_start = lua_gettop(L);

    lua_pushlightuserdata(L, (void *)&REG_KEYS.coroutines); // stack: n+1
    lua_rawget(L, LUA_REGISTRYINDEX);                       // stack: n+1
    lua_pushthread(L);                                      // stack: n+2
    lua_pushnil(L);                                         // stack: n+3
    lua_rawset(L, -3);                                      // stack: n+1

    lua_pop(L, 1); // stack: n
    ww_assert(lua_gettop(L) == stack_start);
}

static void
process_yield(lua_State *L) {
    // Ensure that this coroutine's yield was valid (i.e. it came from a waywall Lua API function
    // and was not from a user call to coroutine.yield).
    //
    // If the yield was invalid, the coroutine will not have any associated waker and will never be
    // able to resume. It should be destroyed immediately.
    if (waker_lookup(L)) {
        return;
    }

    ww_log(LOG_ERROR, "invalid yield from coroutine %p", L);
    coro_table_del(L);
}

static void *
registry_get(lua_State *L, const char *key) {
    ssize_t stack_start = lua_gettop(L);

    lua_pushlightuserdata(L, (void *)key); // stack: n+1
    lua_rawget(L, LUA_REGISTRYINDEX);      // stack: n+1

    void *data = lua_touserdata(L, -1);

    lua_pop(L, 1); // stack: n
    ww_assert(lua_gettop(L) == stack_start);

    return data;
}

static void
registry_set(lua_State *L, const char *key, void *value) {
    ssize_t stack_start = lua_gettop(L);

    lua_pushlightuserdata(L, (void *)key); // stack: n+1
    lua_pushlightuserdata(L, value);       // stack: n+2
    lua_rawset(L, LUA_REGISTRYINDEX);      // stack: n

    ww_assert(lua_gettop(L) == stack_start);
}

static void
waker_destroy(struct config_vm_waker *waker) {
    coro_table_del(waker->L);
    waker->destroy(waker, waker->data);

    wl_list_remove(&waker->link);
    free(waker);
}

static struct config_vm_waker *
waker_lookup(lua_State *L) {
    ssize_t stack_start = lua_gettop(L);

    lua_pushlightuserdata(L, (void *)&REG_KEYS.coroutines); // stack: n+1
    lua_rawget(L, LUA_REGISTRYINDEX);                       // stack: n+1
    lua_pushthread(L);                                      // stack: n+2
    lua_rawget(L, -2);                                      // stack: n+2

    struct config_vm_waker *waker = lua_touserdata(L, -1);

    lua_pop(L, 2); // stack: 0
    ww_assert(lua_gettop(L) == stack_start);

    return waker;
}

struct config_vm *
config_vm_create() {
    struct config_vm *vm = zalloc(1, sizeof(*vm));

    wl_list_init(&vm->wakers);

    // Create the Lua state.
    vm->L = luaL_newstate();
    if (!vm->L) {
        ww_log(LOG_ERROR, "failed to create new lua state");
        goto fail_newstate;
    }

    // By default, the JIT should be disabled. It can be re-enabled if the user has
    // `experimental.jit = true` in their configuration.
    if (!luaJIT_setmode(vm->L, 0, LUAJIT_MODE_OFF)) {
        ww_log(LOG_WARN, "failed to disable the JIT");
    }

    // Store a reference to the config_vm instance within the Lua registry so that it can be
    // accessed by config_vm_from.
    registry_set(vm->L, &REG_KEYS.config_vm, vm);

    // Create the necessary tables within the Lua registry.
    const char *keys[] = {&REG_KEYS.actions, &REG_KEYS.coroutines, &REG_KEYS.events};
    for (size_t i = 0; i < STATIC_ARRLEN(keys); i++) {
        lua_pushlightuserdata(vm->L, (void *)keys[i]); // stack: 1
        lua_newtable(vm->L);                           // stack: 2
        lua_rawset(vm->L, LUA_REGISTRYINDEX);          // stack: 0
    }

    luaL_openlibs(vm->L);
    lua_atpanic(vm->L, on_lua_panic);

    ww_assert(lua_gettop(vm->L) == 0);

    return vm;

fail_newstate:
    free(vm);
    return NULL;
}

void
config_vm_destroy(struct config_vm *vm) {
    if (vm->profile) {
        free(vm->profile);
    }

    struct config_vm_waker *waker, *tmp;
    wl_list_for_each_safe (waker, tmp, &vm->wakers, link) {
        waker_destroy(waker);
    }

    lua_close(vm->L);
    free(vm);
}

struct config_vm *
config_vm_from(lua_State *L) {
    struct config_vm *vm = registry_get(L, &REG_KEYS.config_vm);
    if (!vm) {
        ww_panic("no config_vm entry in registry");
    }

    return vm;
}

struct wrap *
config_vm_get_wrap(struct config_vm *vm) {
    return registry_get(vm->L, &REG_KEYS.wrap);
}

void
config_vm_set_profile(struct config_vm *vm, const char *profile) {
    vm->profile = strdup(profile);
    check_alloc(vm->profile);
}

void
config_vm_set_wrap(struct config_vm *vm, struct wrap *wrap) {
    registry_set(vm->L, &REG_KEYS.wrap, wrap);
}

struct config_vm_waker *
config_vm_create_waker(lua_State *L, config_vm_waker_destroy_func_t destroy, void *data) {
    struct config_vm *vm = config_vm_from(L);

    struct config_vm_waker *waker = zalloc(1, sizeof(*waker));
    waker->L = L;
    waker->destroy = destroy;
    waker->data = data;

    coro_table_add(L, waker);

    wl_list_insert(&vm->wakers, &waker->link);

    return waker;
}

int
config_vm_exec_bcode(struct config_vm *vm, const unsigned char *bc, size_t bc_size,
                     const char *bc_name) {
    if (luaL_loadbuffer(vm->L, (const char *)bc, bc_size, bc_name) != 0) {
        ww_log(LOG_ERROR, "failed to load buffer '%s'", bc_name);
        return 1;
    }
    if (config_vm_pcall(vm, 0, 0, 0) != 0) {
        ww_log(LOG_ERROR, "failed to exec buffer '%s': %s", bc_name, lua_tostring(vm->L, -1));
        return 1;
    }

    return 0;
}

bool
config_vm_is_thread(lua_State *L) {
    int ret = lua_pushthread(L); // stack: n+1
    lua_pop(L, 1);               // stack: n

    return (ret != 1);
}

int
config_vm_pcall(struct config_vm *vm, int nargs, int nresults, int errfunc) {
    lua_sethook(vm->L, on_debug_hook, LUA_MASKCOUNT, MAX_INSTRUCTIONS);
    int ret = lua_pcall(vm->L, nargs, nresults, errfunc);
    lua_sethook(vm->L, NULL, 0, 0);

    return ret;
}

void
config_vm_register_actions(struct config_vm *vm, lua_State *L) {
    // The provided lua_State must have a table containing all actions as the top value on its
    // stack.
    ssize_t stack_start = lua_gettop(L);

    lua_pushlightuserdata(L, (void *)&REG_KEYS.actions);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);

    ww_assert(lua_gettop(L) == stack_start);
}

void
config_vm_register_event(struct config_vm *vm, lua_State *L, const char *name) {
    // The provided lua_State must have a handler function as the top value on its stack.
    ssize_t stack_start = lua_gettop(L);

    lua_pushlightuserdata(L, (void *)&REG_KEYS.events); // stack: n+1
    lua_rawget(L, LUA_REGISTRYINDEX);                   // stack: n+1
    lua_pushstring(L, name);                            // stack: n+2
    lua_pushvalue(L, -3);                               // stack: n+3
    lua_rawset(L, -3);                                  // stack: n+1
    lua_pop(L, 1);                                      // stack: n

    ww_assert(lua_gettop(L) == stack_start);
}

void
config_vm_register_lib(struct config_vm *vm, const struct luaL_Reg *lib, const char *name) {
    luaL_register(vm->L, name, lib); // stack: n+1
    lua_pop(vm->L, 1);               // stack: n
}

void
config_vm_resume(struct config_vm_waker *waker) {
    // Clear the stack so that the coroutine resumes with no arguments.
    lua_settop(waker->L, 0);

    lua_sethook(waker->L, on_debug_hook, LUA_MASKCOUNT, MAX_INSTRUCTIONS);
    int ret = lua_resume(waker->L, 0);
    lua_sethook(waker->L, NULL, 0, 0);

    switch (ret) {
    case LUA_YIELD:
        process_yield(waker->L);
        waker_destroy(waker);
        return;
    case 0:
        // The coroutine returned a value and cannot be resumed again. Remove it from the list
        // of wakers.
        waker_destroy(waker);
        return;
    default:
        // The coroutine failed. Remove it from the waker list and log the error.
        ww_log(LOG_ERROR, "failed to resume coroutine: '%s'", lua_tostring(waker->L, -1));
        waker_destroy(waker);
        return;
    }
}

void
config_vm_signal_event(struct config_vm *vm, const char *name) {
    static const int IDX_TABLE = 1;
    static const int IDX_FUNCTION = 2;

    ww_assert(lua_gettop(vm->L) == 0);

    lua_pushlightuserdata(vm->L, (void *)&REG_KEYS.events); // stack: 1
    lua_rawget(vm->L, LUA_REGISTRYINDEX);                   // stack: 1 (IDX_TABLE)

    lua_pushstring(vm->L, name);  // stack: 2
    lua_rawget(vm->L, IDX_TABLE); // stack: 2 (IDX_FUNCTION)
    ww_assert(lua_type(vm->L, IDX_FUNCTION) == LUA_TFUNCTION);

    if (config_vm_pcall(vm, 0, 0, 0) != 0) {
        ww_log(LOG_ERROR, "failed to signal event '%s': %s", name, lua_tostring(vm->L, -1));
        lua_pop(vm->L, 1); // stack: 1
    }

    lua_pop(vm->L, 1); // stack: 0
    ww_assert(lua_gettop(vm->L) == 0);
}

bool
config_vm_try_action(struct config_vm *vm, size_t index) {
    ww_assert(lua_gettop(vm->L) == 0);

    // Create a new coroutine (Lua thread) and place it in the global coroutines table so that
    // it does not get garbage collected.
    lua_State *coro = lua_newthread(vm->L); // stack: 1
    coro_table_add(coro, NULL);

    // Retrieve the given action function from the actions table.
    lua_pushlightuserdata(coro, (void *)&REG_KEYS.actions); // stack: 1
    lua_rawget(coro, LUA_REGISTRYINDEX);                    // stack: 1
    lua_pushinteger(coro, index);                           // stack: 2
    lua_rawget(coro, 1);                                    // stack: 2

    // Call the function on the new coroutine.
    int ret = lua_resume(coro, 0);
    bool consumed = true;

    switch (ret) {
    case LUA_YIELD:
        process_yield(coro);
        break;
    case 0:
        // The coroutine finished immediately without yielding. Check the function's return
        // value and remove the coroutine from the global table.
        if (lua_gettop(coro) == 0) {
            lua_pushnil(coro);
        }
        consumed = (!lua_isboolean(coro, -1) || lua_toboolean(coro, -1));

        coro_table_del(coro);
        break;
    default:
        // The coroutine failed and threw an error. Remove it from the global table and log the
        // error.
        ww_log(LOG_ERROR, "failed to start action: '%s'", lua_tostring(coro, -1));
        coro_table_del(coro);
        break;
    }

    lua_pop(vm->L, 1); // stack: 0
    ww_assert(lua_gettop(vm->L) == 0);

    return consumed;
}
