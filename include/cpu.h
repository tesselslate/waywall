#ifndef __CPU_H
#define __CPU_H

#include <stdbool.h>
#include <unistd.h>

enum cpu_group {
    CPU_NONE,
    CPU_IDLE,
    CPU_LOW,
    CPU_HIGH,
    CPU_ACTIVE,
};

bool cpu_init();
void cpu_move_to_group(pid_t pid, enum cpu_group group);
bool cpu_set_group_weight(enum cpu_group group, int weight);
void cpu_unset_active();

#endif
