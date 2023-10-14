#ifndef WAYWALL_COMPOSITOR_PUB_WINDOW_UTIL_H
#define WAYWALL_COMPOSITOR_PUB_WINDOW_UTIL_H

struct window;

/*
 *  Sends a request to the given window to close.
 */
void window_close(struct window *window);

/*
 *  Returns the title of the window.
 */
const char *window_get_name(struct window *window);

/*
 *  Returns the ID of the process that created the window.
 */
int window_get_pid(struct window *window);

#endif
