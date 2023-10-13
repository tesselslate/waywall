#ifndef __COMPOSITOR_H
#define __COMPOSITOR_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>
#include <xkbcommon/xkbcommon.h>

#define HEADLESS_WIDTH 1920
#define HEADLESS_HEIGHT 1080

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

typedef bool (*compositor_allow_configure_func_t)(struct window *, int16_t, int16_t);
typedef void (*compositor_button_func_t)(struct compositor_button_event event);
typedef bool (*compositor_key_func_t)(struct compositor_key_event event);
typedef void (*compositor_modifiers_func_t)(uint32_t modifiers);
typedef void (*compositor_motion_func_t)(struct compositor_motion_event event);
typedef void (*compositor_resize_func_t)(int32_t width, int32_t height);
typedef void (*compositor_window_func_t)(struct window *window, bool map);

struct compositor_config {
    int repeat_rate, repeat_delay;
    float floating_opacity;
    float background_color[4];
    bool confine_pointer;
    const char *cursor_theme;
    int cursor_size;
    bool stop_on_close;
};

struct compositor_vtable {
    compositor_allow_configure_func_t allow_configure;
    compositor_button_func_t button;
    compositor_key_func_t key;
    compositor_modifiers_func_t modifiers;
    compositor_motion_func_t motion;
    compositor_resize_func_t resize;
    compositor_window_func_t window;
};

enum compositor_wintype {
    UNKNOWN,
    INSTANCE,
    FLOATING,
};

// Attempts to create a new compositor. Returns NULL on failure.
struct compositor *compositor_create(struct compositor_vtable, struct compositor_config);

// Releases resources associated with `compositor`.
void compositor_destroy(struct compositor *);

// Returns the internal event loop of `compositor`, to which various callbacks can be added.
struct wl_event_loop *compositor_get_loop(struct compositor *);

// Runs the compositor event loop. Returns true on success, false on failure.
bool compositor_run(struct compositor *, int);

// Stops the compositor event loop.
void compositor_stop(struct compositor *);

// Clicks the given window.
void compositor_click(struct window *);

// Returns the number of existing windows. If there are more than 0 windows, a buffer is allocated
// and placed in the user-provided `windows` pointer. The caller must free the buffer.
int compositor_get_windows(struct compositor *, struct window ***);

// Updates user-defined settings for the compositor.
void compositor_load_config(struct compositor *, struct compositor_config);

// Recreates the Wayland output if it is destroyed.
bool compositor_recreate_output(struct compositor *);

// Sends a sequence of keyboard inputs to the given `window`.
void compositor_send_keys(struct window *, const struct compositor_key *, int);

// Sets the mouse sensitivity.
void compositor_set_mouse_sensitivity(struct compositor *, double);

/*
 *  WINDOWS
 */

// Toggles whether or not the user should be allowed to click on an instance to focus it.
void compositor_allow_instance_focus(struct compositor *, bool);

// Requests the client owning the given window to close it.
void compositor_window_close(struct window *);

// Configures the given window to the given position and size.
void compositor_window_configure(struct window *, int16_t, int16_t);

// Destroys all headless views for the given window.
void compositor_window_destroy_headless_views(struct window *);

// Transfers input focus to the given window. If `window` is NULL, input focus is removed from
// whichever window currently has it (if any).
void compositor_window_focus(struct compositor *, struct window *);

// Attempts to get the process ID of the given `window`. Returns -1 on failure.
pid_t compositor_window_get_pid(struct window *);

// Attempts to get the title of the given `window`.
const char *compositor_window_get_name(struct window *);

// Gets the size of the given `window`.
void compositor_window_get_size(struct window *, int16_t *, int16_t *);

// Returns whether or not the given window is focused.
bool compositor_window_is_focused(struct window *);

// Returns whether or not the given window is floating.
bool compositor_window_is_floating(struct window *);

// Creates a new headless view for the given window.
struct headless_view *compositor_window_make_headless_view(struct window *);

// Sets the location and size of the window on the output.
void compositor_window_set_dest(struct window *, struct wlr_box);

// Sets the opacity of the window.
void compositor_window_set_opacity(struct window *, float);

// Sets the type of the given window.
void compositor_window_set_type(struct window *, enum compositor_wintype);

// Sets the visibility of the given window.
void compositor_window_set_visible(struct window *, bool);

// Toggles the visibility of floating windows.
void compositor_toggle_floating(struct compositor *, bool);

/*
 *  HEADLESS VIEWS
 */

// Configures a headless view's size and position on the output.
void compositor_hview_set_dest(struct headless_view *, struct wlr_box);

// Configures the portion of the window which is captured by the headless view.
void compositor_hview_set_src(struct headless_view *, struct wlr_box);

// Moves this headless view to the top of the stack.
void compositor_hview_set_top(struct headless_view *);

/*
 *  RECTANGLES
 */

// Configures a rectangle on the compositor scene.
void compositor_rect_configure(struct wlr_scene_rect *, struct wlr_box);

// Creates a rectangle on the compositor scene.
struct wlr_scene_rect *compositor_rect_create(struct compositor *, struct wlr_box, float[4]);

// Sets the color of a rectangle on the compositor scene.
void compositor_rect_set_color(struct wlr_scene_rect *, float[4]);

// Toggles the visibility of a rectangle on the compositor scene.
void compositor_rect_toggle(struct wlr_scene_rect *, bool);

// Disables or enables all rectangles.
void compositor_toggle_rectangles(struct compositor *, bool);

#endif
