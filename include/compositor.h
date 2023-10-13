#ifndef WAYWALL_COMPOSITOR_H
#define WAYWALL_COMPOSITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <xkbcommon/xkbcommon.h>

#define HEADLESS_WIDTH 1920
#define HEADLESS_HEIGHT 1080

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

typedef bool (*compositor_allow_configure_func_t)(struct window *window, int16_t width,
                                                  int16_t height);
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
struct compositor *compositor_create(struct compositor_vtable vtable,
                                     struct compositor_config config);

// Releases resources associated with `compositor`.
void compositor_destroy(struct compositor *compositor);

// Returns the internal event loop of `compositor`, to which various callbacks can be added.
struct wl_event_loop *compositor_get_loop(struct compositor *compositor);

// Runs the compositor event loop. Returns true on success, false on failure.
bool compositor_run(struct compositor *compositor, int display_file_fd);

// Stops the compositor event loop.
void compositor_stop(struct compositor *compositor);

// Clicks the given window.
void compositor_click(struct window *window);

// Returns the number of existing windows. If there are more than 0 windows, a buffer is allocated
// and placed in the user-provided `windows` pointer. The caller must free the buffer.
int compositor_get_windows(struct compositor *compositor, struct window ***windows);

// Updates user-defined settings for the compositor.
void compositor_load_config(struct compositor *compositor, struct compositor_config config);

// Recreates the Wayland output if it is destroyed.
bool compositor_recreate_output(struct compositor *compositor);

// Sends a sequence of keyboard inputs to the given `window`.
void compositor_send_keys(struct window *window, const struct compositor_key *keys, int count);

// Sets the mouse sensitivity.
void compositor_set_mouse_sensitivity(struct compositor *compositor, double multiplier);

// Notify the compositor if the user is on the wall.
void compositor_set_on_wall(struct compositor *compositor, bool state);

/*
 *  WINDOWS
 */

// Requests the client owning the given window to close it.
void compositor_window_close(struct window *window);

// Configures the given window to the given position and size.
void compositor_window_configure(struct window *, int16_t width, int16_t height);

// Destroys all headless views for the given window.
void compositor_window_destroy_headless_views(struct window *window);

// Transfers input focus to the given window. If `window` is NULL, input focus is removed from
// whichever window currently has it (if any).
void compositor_window_focus(struct compositor *compositor, struct window *window);

// Attempts to get the process ID of the given `window`. Returns -1 on failure.
pid_t compositor_window_get_pid(struct window *window);

// Attempts to get the title of the given `window`.
const char *compositor_window_get_name(struct window *window);

// Gets the size of the given `window`.
void compositor_window_get_size(struct window *window, int16_t *width, int16_t *height);

// Returns whether or not the given window is focused.
bool compositor_window_is_focused(struct window *window);

// Returns whether or not the given window is floating.
bool compositor_window_is_floating(struct window *window);

// Creates a new headless view for the given window.
struct headless_view *compositor_window_make_headless_view(struct window *window);

// Sets the location and size of the window on the output.
void compositor_window_set_dest(struct window *window, struct wlr_box box);

// Sets the opacity of the window.
void compositor_window_set_opacity(struct window *window, float opacity);

// Sets the type of the given window.
void compositor_window_set_type(struct window *window, enum compositor_wintype type);

// Sets the visibility of the given window.
void compositor_window_set_visible(struct window *window, bool visible);

// Toggles the visibility of floating windows.
void compositor_toggle_floating(struct compositor *compositor, bool state);

/*
 *  HEADLESS VIEWS
 */

// Configures a headless view's size and position on the output.
void compositor_hview_set_dest(struct headless_view *view, struct wlr_box box);

// Configures the portion of the window which is captured by the headless view.
void compositor_hview_set_src(struct headless_view *view, struct wlr_box box);

// Moves this headless view to the top of the stack.
void compositor_hview_set_top(struct headless_view *view);

/*
 *  RECTANGLES
 */

// Configures a rectangle on the compositor scene.
void compositor_rect_configure(struct wlr_scene_rect *rect, struct wlr_box box);

// Creates a rectangle on the compositor scene.
struct wlr_scene_rect *compositor_rect_create(struct compositor *compositor, struct wlr_box box,
                                              float color[4]);

// Sets the color of a rectangle on the compositor scene.
void compositor_rect_set_color(struct wlr_scene_rect *rect, float color[4]);

// Toggles the visibility of a rectangle on the compositor scene.
void compositor_rect_toggle(struct wlr_scene_rect *rect, bool state);

// Disables or enables all rectangles.
void compositor_toggle_rectangles(struct compositor *rect, bool state);

#endif
