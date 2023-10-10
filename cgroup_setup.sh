#!/bin/env sh
# To be run with root for using the CPU management features of waywall.
CGROUP_DIR=/sys/fs/cgroup/waywall
USERNAME=$(logname)

if [ ! -d $CGROUP_DIR ]; then
    mkdir $CGROUP_DIR
    mount -t cgroup2 none $CGROUP_DIR
fi

chown "$USERNAME" $CGROUP_DIR
chown "$USERNAME" $CGROUP_DIR/cgroup.procs
echo "+cpu" > $CGROUP_DIR/cgroup.subtree_control

for subgroup in idle low high active; do
    mkdir $CGROUP_DIR/$subgroup
    chown "$USERNAME" $CGROUP_DIR/$subgroup
    chown "$USERNAME" $CGROUP_DIR/$subgroup/cgroup.procs
    chown "$USERNAME" $CGROUP_DIR/$subgroup/cpu.weight
done
