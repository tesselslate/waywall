#include "cmd.h"
#include "string.h"
#include "util/log.h"
#include "util/prelude.h"
#include <bits/types/struct_sched_param.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>

static void
print_help(const char *argv0) {
    static const char *lines[] = {
        "\nUsage:",
        "\twaywall cpu                    Setup cgroups for waywall. Requires root privileges",
        "\twaywall exec -- CMD            Run the specified command in an active waywall instance",
        "\twaywall run [OPTS...]          Start a new instance of waywall",
        "\twaywall wrap [OPTS...] -- CMD  Run the specified command in a new waywall instance",
        "\nOptions:",
        "\t--profile PROFILE              Run waywall with the given configuration profile",
        "",
    };

    for (size_t i = 0; i < STATIC_ARRLEN(lines); i++) {
        fprintf(stderr, "%s\n", lines[i]);
    }
}

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

    if (argc < 2) {
        print_help(argv[0]);
        return 1;
    }

    const char *action = argv[1];
    const char *profile = NULL;
    char **subcommand = NULL;

    bool expect_profile = false;
    for (int i = 2; i < argc; i++) {
        const char *arg = argv[i];

        if (expect_profile) {
            profile = arg;
            expect_profile = false;
        } else {
            if (arg[0] == '-') {
                if (strcmp(arg, "--profile") == 0) {
                    if (profile) {
                        fprintf(stderr, "can only choose one profile\n");
                        return 1;
                    }
                    expect_profile = true;
                } else if (strcmp(arg, "--") == 0) {
                    subcommand = argv + i + 1;
                    break;
                }
            } else {
                print_help(argv[0]);
                return 1;
            }
        }
    }
    if (expect_profile) {
        fprintf(stderr, "expected PROFILE after --profile\n");
        return 1;
    }

    if (strcmp(action, "cpu") == 0) {
        return cmd_cpu();
    } else if (strcmp(action, "exec") == 0) {
        if (!subcommand || !*subcommand) {
            print_help(argv[0]);
            return 1;
        }

        return cmd_exec(subcommand);
    } else if (strcmp(action, "run") == 0) {
        set_realtime();
        log_sysinfo();
        return cmd_run(profile);
    } else if (strcmp(action, "wrap") == 0) {
        if (!subcommand || !*subcommand) {
            print_help(argv[0]);
            return 1;
        }

        set_realtime();
        log_sysinfo();
        return cmd_wrap(profile, subcommand);
    } else {
        print_help(argv[0]);
        return 1;
    }
}
