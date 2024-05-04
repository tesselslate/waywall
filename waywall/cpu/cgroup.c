#include "cpu/cgroup.h"
#include "config/config.h"
#include "cpu/cgroup_setup.h"
#include "cpu/cpu.h"
#include "instance.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/prelude.h"
#include "util/str.h"
#include "wall.h"
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    char *base;

    struct {
        int preview_threshold;
    } config;

    int fds[4];
    int last_active;
    struct {
        enum group group;
        pid_t pid;
        bool priority; // TODO: this wastes a ton of space with padding
    } instances[MAX_INSTANCES];
};

static int
open_group_procs(const char *base, const char *group) {
    str path = str_new();
    str_append(&path, base);
    str_append(&path, group);
    str_append(&path, "/cgroup.procs");

    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd == -1) {
        ww_log_errno(LOG_ERROR, "failed to open '%s'", path);
        str_free(path);
        return 1;
    }
    str_free(path);

    return fd;
}

static int
set_group_weight(const char *base, const char *group, int weight) {
    str path = str_new();
    str_append(&path, base);
    str_append(&path, group);
    str_append(&path, "/cpu.weight");

    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd == -1) {
        ww_log_errno(LOG_ERROR, "failed to open '%s'", path);
        str_free(path);
        return 1;
    }

    char weight_buf[32];
    size_t n = snprintf(weight_buf, STATIC_ARRLEN(weight_buf), "%d", weight);
    ww_assert(n <= STATIC_STRLEN(weight_buf));

    if (write(fd, weight_buf, n) != (ssize_t)n) {
        ww_log_errno(LOG_ERROR, "failed to write '%s'", path);
        str_free(path);
        close(fd);
        return 1;
    }

    str_free(path);
    close(fd);
    return 0;
}

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

    free(cpu->base);
    free(cpu);
}

static int
cpu_cgroup_set_config(struct cpu_manager *manager, struct config *cfg) {
    struct cpu_cgroup *cpu = (struct cpu_cgroup *)manager;

    const struct {
        const char *name;
        int weight;
    } groups[] = {
        {"idle", cfg->cpu.weight_idle},
        {"low", cfg->cpu.weight_low},
        {"high", cfg->cpu.weight_high},
        {"active", cfg->cpu.weight_active},
    };

    for (size_t i = 0; i < STATIC_ARRLEN(groups); i++) {
        if (set_group_weight(cpu->base, groups[i].name, groups[i].weight) != 0) {
            return 1;
        }
    }

    cpu->config.preview_threshold = cfg->cpu.preview_threshold;

    return 0;
}

static void
cpu_cgroup_add(struct cpu_manager *manager, int id, struct instance *instance) {
    struct cpu_cgroup *cpu = (struct cpu_cgroup *)manager;

    cpu->instances[id].group = G_NONE;
    cpu->instances[id].pid = instance->pid;
    cpu->instances[id].priority = false;
}

static void
cpu_cgroup_remove(struct cpu_manager *manager, int id) {
    struct cpu_cgroup *cpu = (struct cpu_cgroup *)manager;

    ww_assert(id != cpu->last_active);
    memmove(cpu->instances + id, cpu->instances + id + 1,
            sizeof(cpu->instances[0]) * (STATIC_ARRLEN(cpu->instances) - id - 1));
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
cpu_cgroup_set_priority(struct cpu_manager *manager, int id, bool priority) {
    struct cpu_cgroup *cpu = (struct cpu_cgroup *)manager;

    cpu->instances[id].priority = priority;
    if (priority && cpu->instances[id].group < G_HIGH) {
        set_group(cpu, id, G_HIGH);
    }
}

static void
cpu_cgroup_update(struct cpu_manager *manager, int id, struct instance *instance) {
    struct cpu_cgroup *cpu = (struct cpu_cgroup *)manager;

    enum group group = G_NONE;
    switch (instance->state.screen) {
    case SCREEN_TITLE:
    case SCREEN_GENERATING:
    case SCREEN_WAITING:
        group = G_HIGH;
        break;
    case SCREEN_PREVIEWING:
        if (cpu->instances[id].priority) {
            group = G_HIGH;
        } else {
            group = (instance->state.data.percent < cpu->config.preview_threshold) ? G_HIGH : G_LOW;
        }
        break;
    case SCREEN_INWORLD:
        group = (cpu->last_active == id) ? G_ACTIVE : G_IDLE;
        break;
    }

    if (group != G_NONE && group != cpu->instances[id].group) {
        set_group(cpu, id, group);
    }
}

struct cpu_manager *
cpu_manager_create_cgroup(struct config *cfg) {
    char *cgroup_base = cgroup_get_base();
    if (!cgroup_base) {
        ww_log(LOG_ERROR, "failed to get cgroup base directory");
        return NULL;
    }

    struct cpu_cgroup *cpu = zalloc(1, sizeof(*cpu));
    cpu->base = cgroup_base;

    if (cpu_cgroup_set_config((struct cpu_manager *)cpu, cfg) != 0) {
        goto fail_set_config;
    }

    static const char *names[] = {"idle", "low", "high", "active"};
    static_assert(STATIC_ARRLEN(names) == STATIC_ARRLEN(cpu->fds));

    for (size_t i = 0; i < STATIC_ARRLEN(names); i++) {
        cpu->fds[i] = open_group_procs(cpu->base, names[i]);

        if (cpu->fds[i] == -1) {
            for (size_t j = 0; j < i; j++) {
                close(cpu->fds[j]);
            }
            goto fail_group;
        }
    }

    cpu->last_active = -1;

    cpu->manager.destroy = cpu_cgroup_destroy;
    cpu->manager.set_config = cpu_cgroup_set_config;

    cpu->manager.add = cpu_cgroup_add;
    cpu->manager.remove = cpu_cgroup_remove;
    cpu->manager.set_active = cpu_cgroup_set_active;
    cpu->manager.set_priority = cpu_cgroup_set_priority;
    cpu->manager.update = cpu_cgroup_update;

    return (struct cpu_manager *)cpu;

fail_group:
fail_set_config:
    free(cpu->base);
    free(cpu);
    return NULL;
}
