#ifndef WAYWALL_CPU_CPU_H
#define WAYWALL_CPU_CPU_H

struct instance;

struct cpu_manager {
    void (*destroy)(struct cpu_manager *cpu);

    void (*death)(struct cpu_manager *cpu, int id);
    void (*set_active)(struct cpu_manager *cpu, int id);
    void (*update)(struct cpu_manager *cpu, int id, struct instance *instance);
};

static inline void
cpu_destroy(struct cpu_manager *cpu) {
    cpu->destroy(cpu);
}

static inline void
cpu_notify_death(struct cpu_manager *cpu, int id) {
    cpu->death(cpu, id);
}

static inline void
cpu_set_active(struct cpu_manager *cpu, int id) {
    cpu->set_active(cpu, id);
}

static inline void
cpu_update(struct cpu_manager *cpu, int id, struct instance *instance) {
    cpu->update(cpu, id, instance);
}

#endif
