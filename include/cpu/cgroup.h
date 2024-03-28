#ifndef WAYWALL_CPU_CGROUP_H
#define WAYWALL_CPU_CGROUP_H

#include "cpu/cpu.h"

struct cpu_manager *cpu_manager_create_cgroup(struct config *cfg);

#endif
