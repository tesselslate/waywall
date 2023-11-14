#ifndef WAYWALL_LAYOUT_H
#define WAYWALL_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>

/*
 *  layout contains a list of items to display to the user, such as instances and rectangles.
 */
struct layout {
    struct layout_entry *entries;
    size_t entry_count;
};

/*
 *  layout_entry contains a single item to be displayed to the user.
 */
struct layout_entry {
    enum layout_entry_type {
        INSTANCE,
        RECTANGLE,
    } type;
    int x, y, w, h;
    union {
        int instance;
        float color[4];
    } data;
};

/*
 *  Frees any resources associated with the given layout.
 */
void layout_destroy(struct layout layout);

/*
 *  Cleans up any resources allocated by the layout module.
 */
void layout_fini();

/*
 *  Initializes data used by the layout module.
 */
bool layout_init();

/*
 *  Requests a new layout with the current state of each instance.
 */
struct layout layout_request_new();

/*
 *  Attempts to reinitialize the layout generator.
 */
bool layout_reinit();

#endif
