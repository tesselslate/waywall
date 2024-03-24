#ifndef WAYWALL_CPU_CGROUP_SETUP_H
#define WAYWALL_CPU_CGROUP_SETUP_H

char *cgroup_get_base();
char *cgroup_get_base_systemd();
int cgroup_setup_check(const char *base);
int cgroup_setup_dir(const char *base);

#endif
