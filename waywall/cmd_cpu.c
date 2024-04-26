#include "cmd.h"
#include "cpu/cgroup_setup.h"
#include "util/log.h"
#include <stdlib.h>

int
cmd_cpu() {
    char *cgroup_base = cgroup_get_base();
    if (!cgroup_base) {
        ww_log(LOG_ERROR, "failed to get cgroup base directory");
        return 1;
    }

    int ret = cgroup_setup_dir(cgroup_base);
    free(cgroup_base);
    return ret;
}
