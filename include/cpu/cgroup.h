#ifndef WAYWALL_CPU_CGROUP_H
#define WAYWALL_CPU_CGROUP_H

#include "cpu/cpu.h"

struct cpu_cgroup_weights {
    int idle;
    int low;
    int high;
    int active;
};

struct cpu_manager *cpu_manager_create_cgroup(struct cpu_cgroup_weights weights);

#endif
