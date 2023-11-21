#ifndef WAYWALL_INSTANCE_H
#define WAYWALL_INSTANCE_H

#include "cpu.h"
#include <stdbool.h>
#include <stdint.h>
#include <sys/inotify.h>
#include <time.h>

#define MIN_CHUNKMAP_VERSION 14

/*
 *  state contains information about the current state of an instance, such as whether it is on the
 *  title screen or in a world.
 */
struct state {
    enum {
        TITLE,
        WAITING,
        GENERATING,
        PREVIEWING,
        INWORLD,
    } screen;
    union {
        int percent;
        enum {
            UNPAUSED,
            PAUSED,
            MENU,
        } inworld;
    } data;
};

/*
 *  instance contains state necessary for managing a particular Minecraft instance, such as its
 *  window handle, options, and installed mods.
 */
struct instance {
    // Instance metadata
    struct window *window;
    char *dir;
    int version;

    struct instance_options {
        struct instance_hotkeys {
            uint8_t atum_reset;
            uint8_t fullscreen;
            uint8_t leave_preview;
        } hotkeys;

        int gui_scale;
        bool unicode;
    } options;

    struct instance_mods {
        bool atum : 1;
        bool standard_settings : 1;
        bool state_output : 1;
        bool world_preview : 1;
    } mods;

    // Instance state
    int state_fd, state_wd, dir_wd;
    int id;
    struct state state;
    bool reset_wait; // When the instance has been reset by a player action but is not yet
                     // generating a new world
    struct timespec last_preview;
};

/*
 *  Frees all resources associated with the given instance.
 */
void instance_destroy(struct instance *instance);

/*
 *  Switches input focus to the given instance and presses any necessary keys to get it to a
 *  playable state, assuming that the instance is in a valid state to be played. Reconfiguring the
 *  window is left to the caller. Returns whether or not the instance was in a valid state to be
 *  given focus.
 */
bool instance_focus(struct instance *instance);

/*
 *  Inspects the given inotify_event and processes it if it is for the given instance. Returns
 *  whether or not the event was processed by the instance.
 */
bool instance_process_inotify(struct instance *instance, const struct inotify_event *event);

/*
 *  Attempts to read the instance's options.txt file. If successful, any changes are reflected in
 *  the `instance` object. If unsuccessful, false is returned and the `options` member of `instance`
 *  is left unchanged.
 */
bool instance_read_options(struct instance *instance);

/*
 *  Resets the given instance if it is in a valid state for resetting. Returns whether or not the
 *  instance was in a valid state for resetting.
 */
bool instance_reset(struct instance *instance);

/*
 *  instance_try_from attempts to create an instance from the given window handle, using the inotify
 *  file descriptor to create any necessary watches. If an error occurs, the value pointed to by
 *  `err` will be set to true.
 */
struct instance instance_try_from(struct window *window, bool *err);

#endif
