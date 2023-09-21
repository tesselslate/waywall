#ifndef __COMPOSITOR_H
#define __COMPOSITOR_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

struct compositor_button_event {
    uint32_t button;
    uint32_t time_msec;
    bool state;
};

struct compositor_key_event {
    const xkb_keysym_t *syms;
    int nsyms;
    uint32_t modifiers;
    uint32_t time_msec;
    bool state;
};

struct compositor_motion_event {
    double x, y;
    uint32_t time_msec;
};

typedef bool (*compositor_button_func_t)(struct compositor_button_event event);
typedef bool (*compositor_key_func_t)(struct compositor_key_event event);
typedef void (*compositor_motion_func_t)(struct compositor_motion_event event);

struct compositor_vtable {
    compositor_button_func_t button;
    compositor_key_func_t key;
    compositor_motion_func_t motion;
};

struct compositor *compositor_create(struct compositor_vtable vtable);
void compositor_destroy(struct compositor *compositor);
struct wl_event_loop *compositor_get_loop(struct compositor *compositor);
bool compositor_run(struct compositor *compositor);

#endif
