#include "util/debug.h"
#include "util/log.h"
#include "util/prelude.h"
#include <inttypes.h>
#include <stdio.h>

bool util_debug_enabled = false;
struct util_debug util_debug_data = {0};

static char debug_buf[524288] = {0};
static FILE *debug_file = NULL;

static void
dbg_keyboard() {
    fprintf(debug_file, "keyboard:\n");
    fprintf(debug_file, "  num_pressed:            %zd\n", util_debug_data.keyboard.num_pressed);
    fprintf(debug_file, "  remote_mods_serialized: 0x%" PRIx32 "\n",
            util_debug_data.keyboard.remote_mods_serialized);
    fprintf(debug_file, "  remote_mods_depressed:  0x%" PRIx32 "\n",
            util_debug_data.keyboard.remote_mods_depressed);
    fprintf(debug_file, "  remote_mods_latched:    0x%" PRIx32 "\n",
            util_debug_data.keyboard.remote_mods_latched);
    fprintf(debug_file, "  remote_mods_locked:     0x%" PRIx32 "\n",
            util_debug_data.keyboard.remote_mods_locked);
    fprintf(debug_file, "  remote_group:           %" PRIu32 "\n",
            util_debug_data.keyboard.remote_group);
    fprintf(debug_file, "  remote_repeat_rate:     %" PRIi32 "\n",
            util_debug_data.keyboard.remote_repeat_rate);
    fprintf(debug_file, "  remote_repeat_delay:    %" PRIi32 "\n",
            util_debug_data.keyboard.remote_repeat_delay);
    fprintf(debug_file, "  active:                 %s\n",
            util_debug_data.keyboard.active ? "yes" : "no");
}

static void
dbg_pointer() {
    fprintf(debug_file, "pointer:\n");
    fprintf(debug_file, "  x:      %lf\n", util_debug_data.pointer.x);
    fprintf(debug_file, "  y:      %lf\n", util_debug_data.pointer.y);
    fprintf(debug_file, "  active: %s\n", util_debug_data.pointer.active ? "yes" : "no");
}

static void
dbg_ui() {
    fprintf(debug_file, "ui:\n");
    fprintf(debug_file, "  width:      %" PRIi32 "\n", util_debug_data.ui.width);
    fprintf(debug_file, "  height:     %" PRIi32 "\n", util_debug_data.ui.height);
    fprintf(debug_file, "  fullscreen: %s\n", util_debug_data.ui.fullscreen ? "yes" : "no");
}

bool
util_debug_init() {
    debug_file = fmemopen(debug_buf, STATIC_STRLEN(debug_buf), "wb");
    if (!debug_file) {
        ww_log_errno(LOG_ERROR, "failed to open memory-backed file");
        return false;
    }

    return true;
}

const char *
util_debug_str() {
    ww_assert(debug_file);
    ww_assert(fseek(debug_file, 0, SEEK_SET) == 0);

    fprintf(debug_file, "debug enabled\n");
    dbg_keyboard();
    dbg_pointer();
    dbg_ui();
    fwrite("\0", 1, 1, debug_file);

    ww_assert(fflush(debug_file) == 0);
    return debug_buf;
}
