#include "cpu/cgroup.h"
#include "cpu/cgroup_setup.h"
#include "cpu/cpu.h"
#include "instance.h"
#include "util.h"
#include "wall.h"
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

enum group {
    G_NONE,
    G_IDLE,
    G_LOW,
    G_HIGH,
    G_ACTIVE,
};

struct cpu_cgroup {
    struct cpu_manager manager;

    struct {
        int preview_threshold;
    } config;

    int fds[4];
    int last_active;
    struct {
        enum group group;
        pid_t pid;
    } instances[MAX_INSTANCES];
};

static void
set_group(struct cpu_cgroup *cpu, int id, enum group group) {
    static_assert(sizeof(pid_t) <= sizeof(int));

    static const int fd_indices[] = {[G_IDLE] = 0, [G_LOW] = 1, [G_HIGH] = 2, [G_ACTIVE] = 3};

    pid_t pid = cpu->instances[id].pid;
    ww_assert(pid > 0);

    char buf[32];
    size_t n = snprintf(buf, STATIC_ARRLEN(buf), "%d", (int)pid);
    ww_assert(n <= STATIC_STRLEN(buf));

    if (write(cpu->fds[fd_indices[group]], buf, n) != (ssize_t)n) {
        ww_log_errno(LOG_ERROR, "failed to write pid %d to group %d", (int)pid, (int)group);
    }
}

static void
cpu_cgroup_destroy(struct cpu_manager *manager) {
    struct cpu_cgroup *cpu = (struct cpu_cgroup *)manager;

    for (size_t i = 0; i < STATIC_ARRLEN(cpu->fds); i++) {
        close(cpu->fds[i]);
    }

    free(cpu);
}

static void
cpu_cgroup_death(struct cpu_manager *manager, int id) {
    struct cpu_cgroup *cpu = (struct cpu_cgroup *)manager;

    ww_assert(id != cpu->last_active);
    memmove(cpu->instances + id, cpu->instances + id + 1,
            sizeof(cpu->instances[0]) * (STATIC_ARRLEN(cpu->instances) - id - 1));

    // Reset the last instance in the array to its zero value.
    cpu->instances[STATIC_ARRLEN(cpu->instances) - 1].group = G_NONE;
    cpu->instances[STATIC_ARRLEN(cpu->instances) - 1].pid = 0;
}

static void
cpu_cgroup_set_active(struct cpu_manager *manager, int id) {
    struct cpu_cgroup *cpu = (struct cpu_cgroup *)manager;

    if (cpu->last_active >= 0) {
        set_group(cpu, cpu->last_active, G_HIGH);
    }

    cpu->last_active = id;
    if (id >= 0) {
        set_group(cpu, id, G_ACTIVE);
    }
}

static void
cpu_cgroup_update(struct cpu_manager *manager, int id, struct instance *instance) {
    struct cpu_cgroup *cpu = (struct cpu_cgroup *)manager;

    cpu->instances[id].pid = instance->pid;

    // TODO: instance priority (lock)

    enum group group = G_NONE;
    switch (instance->state.screen) {
    case SCREEN_TITLE:
    case SCREEN_GENERATING:
    case SCREEN_WAITING:
        group = G_HIGH;
        break;
    case SCREEN_PREVIEWING:
        group = (instance->state.data.percent < cpu->config.preview_threshold) ? G_HIGH : G_LOW;
        break;
    case SCREEN_INWORLD:
        group = (cpu->last_active == id) ? G_ACTIVE : G_IDLE;
        break;
    }

    if (group != G_NONE && group != cpu->instances[id].group) {
        set_group(cpu, id, group);
    }
}

static int
open_group_procs(const char *base, const char *group) {
    struct str buf = {0};
    ww_assert(str_append(&buf, base));
    ww_assert(str_append(&buf, group));
    ww_assert(str_append(&buf, "/cgroup.procs"));

    int fd = open(buf.data, O_WRONLY);
    if (fd == -1) {
        ww_log_errno(LOG_ERROR, "failed to open '%s'", buf.data);
        return 1;
    }

    return fd;
}

static int
set_group_weight(const char *base, const char *group, int weight) {
    struct str buf = {0};
    ww_assert(str_append(&buf, base));
    ww_assert(str_append(&buf, group));
    ww_assert(str_append(&buf, "/cpu.weight"));

    int fd = open(buf.data, O_WRONLY);
    if (fd == -1) {
        ww_log_errno(LOG_ERROR, "failed to open '%s'", buf.data);
        return 1;
    }

    char weight_buf[32];
    size_t n = snprintf(weight_buf, STATIC_ARRLEN(weight_buf), "%d", weight);
    ww_assert(n <= STATIC_STRLEN(weight_buf));

    if (write(fd, weight_buf, n) != (ssize_t)n) {
        ww_log_errno(LOG_ERROR, "failed to write '%s'", buf.data);
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}

struct cpu_manager *
cpu_manager_create_cgroup(struct cpu_cgroup_weights weights, int preview_threshold) {
    // TODO: Non-systemd support
    char *cgroup_base = cgroup_get_base_systemd();
    if (!cgroup_base) {
        ww_log(LOG_ERROR, "failed to get cgroup base directory");
        return NULL;
    }

    const struct {
        const char *name;
        int weight;
    } groups[] = {
        {"idle", weights.idle},
        {"low", weights.low},
        {"high", weights.high},
        {"active", weights.active},
    };

    for (size_t i = 0; i < STATIC_ARRLEN(groups); i++) {
        if (set_group_weight(cgroup_base, groups[i].name, groups[i].weight) != 0) {
            goto fail_weight;
        }
    }

    struct cpu_cgroup *cpu = calloc(1, sizeof(*cpu));
    if (!cpu) {
        ww_log(LOG_ERROR, "failed to allocate cpu_cgroup");
        goto fail_cpu;
    }

    cpu->config.preview_threshold = preview_threshold;

    static const char *names[] = {"idle", "low", "high", "active"};
    static_assert(STATIC_ARRLEN(names) == STATIC_ARRLEN(cpu->fds));

    for (size_t i = 0; i < STATIC_ARRLEN(names); i++) {
        cpu->fds[i] = open_group_procs(cgroup_base, names[i]);

        if (cpu->fds[i] == -1) {
            for (size_t j = 0; j < i; j++) {
                close(cpu->fds[j]);
            }
            goto fail_group;
        }
    }

    cpu->last_active = -1;

    cpu->manager.destroy = cpu_cgroup_destroy;
    cpu->manager.death = cpu_cgroup_death;
    cpu->manager.set_active = cpu_cgroup_set_active;
    cpu->manager.update = cpu_cgroup_update;

    free(cgroup_base);
    return (struct cpu_manager *)cpu;

fail_group:
    free(cpu);

fail_cpu:
fail_weight:
    free(cgroup_base);
    return NULL;
}
