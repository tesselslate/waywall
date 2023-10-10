#ifndef __CFS_H
#define __CFS_H

#include <stdbool.h>
#include <unistd.h>

enum cfs_group {
    CFS_NONE,
    CFS_IDLE,
    CFS_LOW,
    CFS_HIGH,
    CFS_ACTIVE,
};

bool cfs_init();
void cfs_move_to_group(pid_t pid, enum cfs_group group);
bool cfs_set_group_weight(enum cfs_group group, int weight);

#endif
