#ifndef __COMPOSITOR_H
#define __COMPOSITOR_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

struct compositor;
struct window;

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

struct compositor_key {
    uint8_t keycode;
    bool state;
};

typedef bool (*compositor_button_func_t)(struct compositor_button_event event);
typedef bool (*compositor_key_func_t)(struct compositor_key_event event);
typedef void (*compositor_modifiers_func_t)(uint32_t modifiers);
typedef void (*compositor_motion_func_t)(struct compositor_motion_event event);
typedef bool (*compositor_window_func_t)(struct window *window, bool map);

struct compositor_vtable {
    compositor_button_func_t button;
    compositor_key_func_t key;
    compositor_modifiers_func_t modifiers;
    compositor_motion_func_t motion;
    compositor_window_func_t window;
};

// Attempts to create a new compositor. Returns NULL on failure.
struct compositor *compositor_create(struct compositor_vtable vtable);

// Releases resources associated with `compositor`.
void compositor_destroy(struct compositor *compositor);

// Returns the internal event loop of `compositor`, to which various callbacks can be added.
struct wl_event_loop *compositor_get_loop(struct compositor *compositor);

// Runs the compositor event loop. Returns true on success, false on failure.
bool compositor_run(struct compositor *compositor);

// Stops the compositor event loop.
void compositor_stop(struct compositor *compositor);

// Clicks the given window.
void compositor_click(struct window *window);

// Configures the given window to the given position and size.
void compositor_configure_window(struct window *window, int16_t x, int16_t y, int16_t w, int16_t h);

// Transfers input focus to the given window. If `window` is NULL, input focus is removed from
// whichever window currently has it (if any).
void compositor_focus_window(struct compositor *compositor, struct window *window);

// Returns the size of the compositor's main output.
void compositor_get_screen_size(struct compositor *compositor, int32_t *w, int32_t *h);

// Returns the number of existing windows. If there are more than 0 windows, a buffer is allocated
// and placed in the user-provided `windows` pointer. The caller must free the buffer.
int compositor_get_windows(struct compositor *compositor, struct window ***windows);

// Attempts to get the process ID of the given `window`. Returns -1 on failure.
pid_t compositor_get_window_pid(struct window *window);

// Sends a sequence of keyboard inputs to the given `window`.
void compositor_send_keys(struct window *window, const struct compositor_key *keys, int count);

#endif
