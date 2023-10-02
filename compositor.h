#ifndef __COMPOSITOR_H
#define __COMPOSITOR_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>
#include <xkbcommon/xkbcommon.h>

struct compositor;
struct window;
struct wlr_scene_rect;

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
typedef void (*compositor_resize_func_t)(int32_t width, int32_t height);
typedef bool (*compositor_window_func_t)(struct window *window, bool map);

struct compositor_config {
    int repeat_rate, repeat_delay;
    float background_color[4];
    bool confine_pointer;
    const char *cursor_theme;
    int cursor_size;
};

struct compositor_vtable {
    compositor_button_func_t button;
    compositor_key_func_t key;
    compositor_modifiers_func_t modifiers;
    compositor_motion_func_t motion;
    compositor_resize_func_t resize;
    compositor_window_func_t window;
};

// Attempts to create a new compositor. Returns NULL on failure.
struct compositor *compositor_create(struct compositor_vtable vtable,
                                     struct compositor_config config);

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
void compositor_configure_window(struct window *window, int16_t w, int16_t h);

// Transfers input focus to the given window. If `window` is NULL, input focus is removed from
// whichever window currently has it (if any).
void compositor_focus_window(struct compositor *compositor, struct window *window);

// Returns the number of existing windows. If there are more than 0 windows, a buffer is allocated
// and placed in the user-provided `windows` pointer. The caller must free the buffer.
int compositor_get_windows(struct compositor *compositor, struct window ***windows);

// Attempts to get the process ID of the given `window`. Returns -1 on failure.
pid_t compositor_get_window_pid(struct window *window);

// Updates user-defined settings for the compositor.
void compositor_load_config(struct compositor *compositor, struct compositor_config config);

// Configures a rectangle on the compositor scene.
void compositor_rect_configure(struct wlr_scene_rect *, struct wlr_box);

// Creates a rectangle on the compositor scene.
struct wlr_scene_rect *compositor_rect_create(struct compositor *, struct wlr_box, float[4]);

// Sets the color of a rectangle on the compositor scene.
void compositor_rect_set_color(struct wlr_scene_rect *, float[4]);

// Toggles the visibility of a rectangle on the compositor scene.
void compositor_rect_toggle(struct wlr_scene_rect *, bool);

// Sends a sequence of keyboard inputs to the given `window`.
void compositor_send_keys(struct window *window, const struct compositor_key *keys, int count);

// Sets the mouse sensitivity.
void compositor_set_mouse_sensitivity(struct compositor *compositor, double multiplier);

// Sets the location and size of the window on the output.
void compositor_set_window_render_dest(struct window *window, struct wlr_box);

#endif
