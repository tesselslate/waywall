#ifndef WAYWALL_CPU_CPU_H
#define WAYWALL_CPU_CPU_H

#include <stdbool.h>

struct instance;

struct cpu_manager {
    void (*destroy)(struct cpu_manager *cpu);

    void (*add)(struct cpu_manager *cpu, int id, struct instance *instance);
    void (*remove)(struct cpu_manager *cpu, int id);
    void (*set_active)(struct cpu_manager *cpu, int id);
    void (*set_priority)(struct cpu_manager *cpu, int id, bool priority);
    void (*update)(struct cpu_manager *cpu, int id, struct instance *instance);
};

static inline void
cpu_destroy(struct cpu_manager *cpu) {
    cpu->destroy(cpu);
}

static inline void
cpu_add(struct cpu_manager *cpu, int id, struct instance *instance) {
    cpu->add(cpu, id, instance);
}

static inline void
cpu_remove(struct cpu_manager *cpu, int id) {
    cpu->remove(cpu, id);
}

static inline void
cpu_set_active(struct cpu_manager *cpu, int id) {
    cpu->set_active(cpu, id);
}

static inline void
cpu_set_priority(struct cpu_manager *cpu, int id, bool priority) {
    cpu->set_priority(cpu, id, priority);
}

static inline void
cpu_update(struct cpu_manager *cpu, int id, struct instance *instance) {
    cpu->update(cpu, id, instance);
}

#endif
