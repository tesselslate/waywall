#include "cmd.h"
#include "string.h"
#include "util/log.h"
#include <sched.h>
#include <stdlib.h>
#include <unistd.h>

static void
set_realtime() {
    int priority = sched_get_priority_min(SCHED_RR);
    if (priority == -1) {
        ww_log_errno(LOG_ERROR, "failed to get minimum priority for SCHED_RR");
        return;
    }

    const struct sched_param param = {.sched_priority = priority};
    if (sched_setscheduler(getpid(), SCHED_RR, &param) == -1) {
        ww_log_errno(LOG_ERROR, "failed to set scheduler priority");
        return;
    }
}

int
main(int argc, char **argv) {
    util_log_init();

    if (argc == 1) {
        set_realtime();
        return cmd_waywall();
    }

    if (strcmp(argv[1], "cpu") == 0) {
        return cmd_cpu();
    } else {
        ww_log(LOG_ERROR, "unknown subcommand '%s'", argv[1]);
        return 1;
    }
}
