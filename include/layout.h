#ifndef WAYWALL_LAYOUT_H
#define WAYWALL_LAYOUT_H

#include "util.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INSTANCE_BITFIELD_WIDTH (MAX_INSTANCES / 8)
static_assert(INSTANCE_BITFIELD_WIDTH == 16, "bitfield width is 16");

/*
 *  instance_bitfield contains a bitfield wide enough to represent all instances.
 */
struct instance_bitfield {
    uint8_t bits[INSTANCE_BITFIELD_WIDTH];
};

/*
 *  instance_list contains a list of instance IDs.
 */
struct instance_list {
    uint8_t ids[MAX_INSTANCES];
    size_t id_count;
};

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
 *  layout_reason contains information about why a layout update was triggered.
 */
struct layout_reason {
    enum {
        REASON_INIT,
        REASON_INSTANCE_ADD,
        REASON_INSTANCE_DIE,
        REASON_PREVIEW_START,
        REASON_LOCK,
        REASON_UNLOCK,
        REASON_RESET,
        REASON_RESET_ALL,
        REASON_RESET_INGAME,
        REASON_RESIZE,
    } cause;
    union {
        int screen_size[2];
        int instance_id;
    } data;
};

static inline bool
instance_bitfield_has(struct instance_bitfield bitfield, size_t id) {
    return (bitfield.bits[id / 8] & (1 << (id % 8))) != 0;
}

struct wall;

/*
 *  Frees any resources associated with the given layout.
 */
void layout_destroy(struct layout layout);

/*
 *  Cleans up any resources allocated by the layout module.
 */
void layout_fini();

/*
 *  Requests a list of instance IDs to use for the play first locked keybind.
 */
struct instance_list layout_get_locked(struct wall *wall);

/*
 *  Requests a list of instance IDs to reset when the user presses the reset all keybind.
 */
struct instance_bitfield layout_get_reset_all(struct wall *wall);

/*
 *  Attempts to initialize the layout generator and provide a first layout.
 */
bool layout_init(struct wall *wall, struct layout *layout);

/*
 *  Attempts to reinitialize the layout generator and provide a first layout.
 */
bool layout_reinit(struct wall *wall, struct layout *layout);

/*
 *  Requests a new layout with the current state of each instance. Returns whether or not the active
 *  generator returned a new layout.
 */
bool layout_request_new(struct wall *wall, struct layout_reason reason, struct layout *layout);

#endif
