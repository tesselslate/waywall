#include "compositor.h"
#include "config.h"
#include "waywall.h"
#include <stdbool.h>
#include <wayland-server-core.h>

static struct wl_listener unmap_listener;
static struct wl_listener configure_listener;
static struct wl_listener minimize_listener;
static struct wl_listener output_resize_listener;

static struct window *main_window;
static bool visible = false;
static int width, height;
static int screen_width, screen_height;

static void
reposition() {
    if (!main_window) {
        return;
    }

    if (width <= 0 || height <= 0) {
        render_window_get_size(main_window, &width, &height);
    }

    int x, y;
    enum ninb_location loc = g_config->ninb_location;

    if (loc == TOP_LEFT || loc == LEFT || loc == BOTTOM_LEFT) {
        x = 0;
    } else if (loc == TOP_RIGHT || loc == RIGHT || loc == BOTTOM_RIGHT) {
        x = screen_width - width;
    } else if (loc == TOP) {
        x = (screen_width - width) / 2;
    } else {
        ww_unreachable();
    }

    if (loc == TOP_LEFT || loc == TOP || loc == TOP_RIGHT) {
        y = 0;
    } else if (loc == LEFT || loc == RIGHT) {
        y = (screen_height - height) / 2;
    } else if (loc == BOTTOM_LEFT || loc == BOTTOM_RIGHT) {
        y = screen_height - height;
    } else {
        ww_unreachable();
    }

    render_window_set_pos(main_window, x, y);
}

static void
on_unmap(struct wl_listener *listener, void *data) {
    struct window *window = data;

    if (window == main_window) {
        main_window = NULL;
        visible = false;
        width = height = 0;
    }
}

static void
on_configure(struct wl_listener *listener, void *data) {
    struct window_configure_event *event = data;

    if (event->window == main_window) {
        width = event->box.width;
        height = event->box.height;
        render_window_configure(event->window, 0, 0, event->box.width, event->box.height);
        reposition();
    }
}

static void
on_minimize(struct wl_listener *listener, void *data) {
    struct window_minimize_event *event = data;

    if (event->minimized && event->window == main_window) {
        render_layer_set_enabled(g_compositor->render, LAYER_FLOATING, false);
        visible = false;
    }
}

static void
on_output_resize(struct wl_listener *listener, void *data) {
    struct output *output = data;

    render_output_get_size(output, &screen_width, &screen_height);
    reposition();
}

void
ninb_init() {
    unmap_listener.notify = on_unmap;
    wl_signal_add(&g_compositor->render->events.window_unmap, &unmap_listener);

    configure_listener.notify = on_configure;
    wl_signal_add(&g_compositor->render->events.window_configure, &configure_listener);

    minimize_listener.notify = on_minimize;
    wl_signal_add(&g_compositor->render->events.window_minimize, &minimize_listener);

    output_resize_listener.notify = on_output_resize;
    wl_signal_add(&g_compositor->render->events.wl_output_resize, &output_resize_listener);

    render_layer_set_enabled(g_compositor->render, LAYER_FLOATING, false);
}

void
ninb_toggle() {
    visible = !visible;

    if (visible) {
        reposition();
    }
    render_layer_set_enabled(g_compositor->render, LAYER_FLOATING, visible);
}

bool
ninb_try_window(struct window *window) {
    const char *name = window_get_name(window);

    // The loading window has the title "Java".
    if (strcmp(name, "Java") == 0) {
        if (main_window) {
            window_kill(window);
            return true;
        }
        render_window_set_layer(window, LAYER_FLOATING);
        render_window_set_enabled(window, false);
        return true;
    }

    // The main window will have "Ninjabrain Bot" in the title.
    if (strstr(name, "Ninjabrain Bot")) {
        if (main_window) {
            window_kill(window);
            return true;
        }
        main_window = window;
        reposition();
        render_window_set_layer(window, LAYER_FLOATING);
        render_window_set_enabled(window, true);
        return true;
    }

    // The settings and calibration windows will be owned by the same process.
    if (main_window && window_get_pid(main_window) == window_get_pid(window)) {
        render_window_set_layer(window, LAYER_FLOATING);
        render_window_set_enabled(window, true);
        return true;
    }

    return false;
}

void
ninb_update_config() {
    reposition();
}
