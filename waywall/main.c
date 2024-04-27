#include "cmd.h"
#include "string.h"
#include "util/log.h"
#include "util/prelude.h"
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/utsname.h>
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

static void
log_sysinfo() {
    struct utsname name;
    ww_assert(uname(&name) == 0);

    ww_log(LOG_INFO, "system:  %s", name.sysname);
    ww_log(LOG_INFO, "release: %s", name.release);
    ww_log(LOG_INFO, "version: %s", name.version);
    ww_log(LOG_INFO, "machine: %s", name.machine);

    struct rlimit nofile;
    ww_assert(getrlimit(RLIMIT_NOFILE, &nofile) == 0);

    ww_log(LOG_INFO, "nofile:  %ju / %ju", (uintmax_t)nofile.rlim_cur, (uintmax_t)nofile.rlim_max);
}

int
main(int argc, char **argv) {
    util_log_init();

    if (argc == 1) {
        set_realtime();
        log_sysinfo();
        return cmd_waywall();
    }

    if (strcmp(argv[1], "cpu") == 0) {
        return cmd_cpu();
    } else if (strcmp(argv[1], "exec") == 0) {
        return cmd_exec(argc, argv);
    } else {
        ww_log(LOG_ERROR, "unknown subcommand '%s'", argv[1]);
        return 1;
    }
}
