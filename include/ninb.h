#ifndef WAYWALL_NINB_H
#define WAYWALL_NINB_H

#include "compositor/render.h"
#include <stdbool.h>

/*
 *  Initializes the Ninjabrain Bot module.
 */
void ninb_init();

/*
 *  Toggles the visibility of Ninjabrain Bot.
 */
void ninb_toggle();

/*
 *  Checks whether the given window is a Ninjabrain Bot window. If so, it takes ownership of it.
 */
bool ninb_try_window(struct window *window);

/*
 *  Loads a new configuration.
 */
void ninb_update_config();

#endif
