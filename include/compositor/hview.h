#ifndef WAYWALL_COMPOSITOR_PUB_HVIEW_H
#define WAYWALL_COMPOSITOR_PUB_HVIEW_H

#include "compositor/render.h"
#include <stdbool.h>
#include <wlr/util/box.h>

struct hview;

/*
 *  Creates a new hview from the given window.
 */
struct hview *hview_create(struct window *window);

/*
 *  Frees all resources associated with the hview.
 */
void hview_destroy(struct hview *hview);

/*
 *  Raises this hview above all others.
 */
void hview_raise(struct hview *hview);

/*
 *  Sets the destination box of the hview.
 */
void hview_set_dest(struct hview *hview, struct wlr_box box);

/*
 *  Toggles the visibility of the hview.
 */
void hview_set_enabled(struct hview *hview, bool enabled);

/*
 *  Sets the source crop for the hview.
 */
void hview_set_src(struct hview *hview, struct wlr_box box);

#endif
