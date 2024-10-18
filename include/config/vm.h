#ifndef WAYWALL_CONFIG_VM_H
#define WAYWALL_CONFIG_VM_H

#include <luajit-2.1/lauxlib.h>
#include <luajit-2.1/lua.h>
#include <stdbool.h>
#include <wayland-util.h>

struct config_action;
struct config_vm_waker;

typedef void (*config_vm_waker_destroy_func_t)(struct config_vm_waker *waker, void *data);

struct config_vm {
    lua_State *L;

    char *profile;
    struct wl_list wakers; // config_vm_waker.link
};

struct wrap;

struct config_vm *config_vm_create();
void config_vm_destroy(struct config_vm *vm);

struct config_vm *config_vm_from(lua_State *L);
struct wrap *config_vm_get_wrap(struct config_vm *vm);
void config_vm_set_wrap(struct config_vm *vm, struct wrap *wrap);
void config_vm_set_profile(struct config_vm *vm, const char *profile);

struct config_vm_waker *config_vm_create_waker(lua_State *L, config_vm_waker_destroy_func_t destroy,
                                               void *data);
int config_vm_exec_bcode(struct config_vm *vm, const unsigned char *bc, size_t bc_size,
                         const char *bc_name);
bool config_vm_is_thread(lua_State *L);
int config_vm_pcall(struct config_vm *vm, int nargs, int nresults, int errfunc);
void config_vm_register_actions(struct config_vm *vm, lua_State *L);
void config_vm_register_event(struct config_vm *vm, lua_State *L, const char *name);
void config_vm_register_lib(struct config_vm *vm, const struct luaL_Reg *lib, const char *name);
void config_vm_resume(struct config_vm_waker *waker);
void config_vm_signal_event(struct config_vm *vm, const char *name);
bool config_vm_try_action(struct config_vm *vm, const struct config_action *action);

#endif
