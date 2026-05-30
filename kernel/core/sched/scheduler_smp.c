/*
 * SMP Load-Balanced Scheduler
 * ============================
 *
 * Per-CPU runqueues with active load balancing and work stealing.
 *
 * Architecture:
 *  - Each CPU has its own runqueue (eliminates global lock contention)
 *  - Per-CPU locks for fine-grained locking
 *  - Periodic load balancing: migrate processes from busy → idle CPUs
 *  - Work stealing: idle CPUs steal from busiest CPU
 *  - CPU affinity: processes prefer their last CPU
 *
 * Load Balancing:
 *  - Triggered every LOAD_BALANCE_INTERVAL (50 ticks)
 *  - Migrate if imbalance > LOAD_IMBALANCE_THRESHOLD (2 processes)
 *  - Only migrate processes that haven't run recently (cache-cold)
 *
 * Locking Protocol:
 *  - Each runqueue has its own lock
 *  - Lock ordering: lower CPU ID first (prevents deadlock)
 *  - IRQ-safe locks (disable interrupts while holding)
 */

#ifdef CONFIG_SMP

#include "../../include/sched.h"
#include "../../include/kernel.h"
#include "../../include/x86_64.h"
#include "../../include/smp.h"
#include "../../include/spinlock.h"
#include "../../include/mem.h"

// Load balancing constants
#define LOAD_BALANCE_INTERVAL 50        // Balance every 50 ticks
#define LOAD_IMBALANCE_THRESHOLD 2      // Migrate if difference > 2 processes
#define MIGRATION_COST_THRESHOLD 5      // Don't migrate if process ran < 5 ticks ago
#define DEFAULT_TIME_SLICE 10           // Timer ticks per quantum

// Per-CPU runqueue structure
typedef struct cpu_runqueue {
    process_t* queue_head;              // Head of process list
    process_t* queue_tail;              // Tail of process list
    uint32_t load;                      // Number of processes
    spinlock_t lock;                    // Per-CPU lock
    uint64_t last_balance_tick;         // Last load balance tick
} cpu_runqueue_t;

// Global SMP scheduler state
static cpu_runqueue_t runqueues[MAX_CPUS];
static uint64_t global_tick_count = 0;
static spinlock_t tick_lock;

// Statistics
static uint64_t total_migrations = 0;
static uint64_t total_steals = 0;

// ===========================================================================
// CPU Runqueue Operations
// ===========================================================================

// Initialize a CPU runqueue
static void cpu_runqueue_init(cpu_runqueue_t* rq, uint32_t cpu_id) {
    rq->queue_head = NULL;
    rq->queue_tail = NULL;
    rq->load = 0;
    spin_lock_init(&rq->lock);
    rq->last_balance_tick = 0;
}

// Enqueue process to CPU runqueue (lock must be held)
static void cpu_runqueue_enqueue_locked(cpu_runqueue_t* rq, process_t* proc) {
    proc->next = NULL;

    if (rq->queue_tail == NULL) {
        // Empty queue
        rq->queue_head = proc;
        rq->queue_tail = proc;
    } else {
        // Add to tail
        rq->queue_tail->next = proc;
        rq->queue_tail = proc;
    }

    rq->load++;
    proc->on_queue = 1;
}

// Dequeue process from CPU runqueue (lock must be held)
static process_t* cpu_runqueue_dequeue_locked(cpu_runqueue_t* rq) {
    if (rq->queue_head == NULL) {
        return NULL;
    }

    process_t* proc = rq->queue_head;
    rq->queue_head = proc->next;

    if (rq->queue_head == NULL) {
        rq->queue_tail = NULL;
    }

    proc->next = NULL;
    proc->on_queue = 0;
    rq->load--;

    return proc;
}

// Remove specific process from runqueue (lock must be held)
static int cpu_runqueue_remove_locked(cpu_runqueue_t* rq, process_t* proc) {
    process_t* current = rq->queue_head;
    process_t* prev = NULL;

    while (current != NULL) {
        if (current == proc) {
            // Found it - remove
            if (prev == NULL) {
                rq->queue_head = current->next;
                if (rq->queue_head == NULL) {
                    rq->queue_tail = NULL;
                }
            } else {
                prev->next = current->next;
                if (current == rq->queue_tail) {
                    rq->queue_tail = prev;
                }
            }

            current->next = NULL;
            current->on_queue = 0;
            rq->load--;
            return 1;
        }

        prev = current;
        current = current->next;
    }

    return 0;
}

// ===========================================================================
// Load Balancing
// ===========================================================================

// Find busiest CPU (most processes)
static uint32_t find_busiest_cpu(void) {
    uint32_t busiest = 0;
    uint32_t max_load = 0;
    uint32_t num_cpus = smp_cpu_count();

    for (uint32_t cpu = 0; cpu < num_cpus; cpu++) {
        if (runqueues[cpu].load > max_load) {
            max_load = runqueues[cpu].load;
            busiest = cpu;
        }
    }

    return busiest;
}

// Find idlest CPU (fewest processes)
static uint32_t find_idlest_cpu(void) {
    uint32_t idlest = 0;
    uint32_t min_load = 0xFFFFFFFF;
    uint32_t num_cpus = smp_cpu_count();

    for (uint32_t cpu = 0; cpu < num_cpus; cpu++) {
        if (runqueues[cpu].load < min_load) {
            min_load = runqueues[cpu].load;
            idlest = cpu;
        }
    }

    return idlest;
}

// Migrate a process from one CPU to another
static int migrate_process(uint32_t from_cpu, uint32_t to_cpu) {
    if (from_cpu == to_cpu) {
        return 0;
    }

    cpu_runqueue_t* from_rq = &runqueues[from_cpu];
    cpu_runqueue_t* to_rq = &runqueues[to_cpu];

    // Lock in order (lower CPU ID first to prevent deadlock)
    uint64_t flags1, flags2;
    if (from_cpu < to_cpu) {
        spin_lock_irqsave(&from_rq->lock, &flags1);
        spin_lock_irqsave(&to_rq->lock, &flags2);
    } else {
        spin_lock_irqsave(&to_rq->lock, &flags2);
        spin_lock_irqsave(&from_rq->lock, &flags1);
    }

    // Find a process to migrate (skip recently-run processes)
    process_t* current = from_rq->queue_head;
    process_t* prev = NULL;
    process_t* to_migrate = NULL;

    while (current != NULL) {
        // Check if process is cache-cold (hasn't run recently)
        uint64_t ticks_since_run = global_tick_count - current->total_time;
        if (ticks_since_run >= MIGRATION_COST_THRESHOLD) {
            to_migrate = current;

            // Remove from source queue
            if (prev == NULL) {
                from_rq->queue_head = current->next;
                if (from_rq->queue_head == NULL) {
                    from_rq->queue_tail = NULL;
                }
            } else {
                prev->next = current->next;
                if (current == from_rq->queue_tail) {
                    from_rq->queue_tail = prev;
                }
            }
            from_rq->load--;

            break;
        }

        prev = current;
        current = current->next;
    }

    int migrated = 0;
    if (to_migrate) {
        // Add to destination queue
        cpu_runqueue_enqueue_locked(to_rq, to_migrate);
        total_migrations++;
        migrated = 1;

#ifndef SCHEDULER_QUIET
        kprintf("[SCHEDULER] Migrated process '%s' (PID %d) from CPU %d to CPU %d\n",
                to_migrate->name, to_migrate->pid, from_cpu, to_cpu);
#endif
    }

    // Unlock in reverse order
    if (from_cpu < to_cpu) {
        spin_unlock_irqrestore(&to_rq->lock, flags2);
        spin_unlock_irqrestore(&from_rq->lock, flags1);
    } else {
        spin_unlock_irqrestore(&from_rq->lock, flags1);
        spin_unlock_irqrestore(&to_rq->lock, flags2);
    }

    return migrated;
}

// Perform load balancing across all CPUs
static void load_balance(void) {
    uint32_t num_cpus = smp_cpu_count();
    if (num_cpus <= 1) {
        return;  // Single CPU, no balancing needed
    }

    uint32_t busiest = find_busiest_cpu();
    uint32_t idlest = find_idlest_cpu();

    uint32_t busiest_load = runqueues[busiest].load;
    uint32_t idlest_load = runqueues[idlest].load;

    // Check if imbalance exceeds threshold
    if (busiest_load > idlest_load + LOAD_IMBALANCE_THRESHOLD) {
        // Migrate processes until balanced
        uint32_t target_migrations = (busiest_load - idlest_load) / 2;

        for (uint32_t i = 0; i < target_migrations; i++) {
            if (!migrate_process(busiest, idlest)) {
                break;  // No more migratable processes
            }
        }

#ifndef SCHEDULER_QUIET
        kprintf("[SCHEDULER] Load balanced: CPU %d (%d → %d), CPU %d (%d → %d)\n",
                busiest, busiest_load, runqueues[busiest].load,
                idlest, idlest_load, runqueues[idlest].load);
#endif
    }
}

// Work stealing: idle CPU steals from busiest CPU
static process_t* work_steal(uint32_t idle_cpu) {
    uint32_t num_cpus = smp_cpu_count();
    if (num_cpus <= 1) {
        return NULL;
    }

    uint32_t busiest = find_busiest_cpu();

    // Don't steal from ourselves or from empty queues
    if (busiest == idle_cpu || runqueues[busiest].load == 0) {
        return NULL;
    }

    cpu_runqueue_t* from_rq = &runqueues[busiest];
    uint64_t flags;

    spin_lock_irqsave(&from_rq->lock, &flags);
    process_t* stolen = cpu_runqueue_dequeue_locked(from_rq);
    spin_unlock_irqrestore(&from_rq->lock, flags);

    if (stolen) {
        total_steals++;
#ifndef SCHEDULER_QUIET
        kprintf("[SCHEDULER] CPU %d stole process '%s' (PID %d) from CPU %d\n",
                idle_cpu, stolen->name, stolen->pid, busiest);
#endif
    }

    return stolen;
}

// ===========================================================================
// Scheduler API
// ===========================================================================

void scheduler_init(void) {
    kprintf("[SCHEDULER] Initializing SMP load-balanced scheduler...\n");

    uint32_t num_cpus = smp_cpu_count();
    spin_lock_init(&tick_lock);

    // Initialize per-CPU runqueues
    for (uint32_t cpu = 0; cpu < num_cpus; cpu++) {
        cpu_runqueue_init(&runqueues[cpu], cpu);
    }

    global_tick_count = 0;
    total_migrations = 0;
    total_steals = 0;

    kprintf("[SCHEDULER] SMP scheduler initialized:\n");
    kprintf("[SCHEDULER]   - CPUs: %d\n", num_cpus);
    kprintf("[SCHEDULER]   - Per-CPU runqueues: Yes\n");
    kprintf("[SCHEDULER]   - Load balancing: Every %d ticks\n", LOAD_BALANCE_INTERVAL);
    kprintf("[SCHEDULER]   - Work stealing: Enabled\n");
}

void scheduler_add_process(process_t* proc) {
    if (!proc) {
        kprintf("[SCHEDULER] Warning: Attempted to add NULL process\n");
        return;
    }

    if (proc->state == PROCESS_TERMINATED) {
        kprintf("[SCHEDULER] Warning: Attempted to add terminated process %d\n", proc->pid);
        return;
    }

    // Check if already queued
    if (proc->on_queue) {
        return;
    }

    // Take reference when adding to queue
    process_ref(proc);

    // Determine target CPU (use current CPU or least loaded)
    uint32_t target_cpu = cpu_id();
    uint32_t num_cpus = smp_cpu_count();

    // If process has CPU affinity, try to use it
    // Otherwise, add to current CPU's runqueue
    if (target_cpu >= num_cpus) {
        target_cpu = 0;
    }

    cpu_runqueue_t* rq = &runqueues[target_cpu];
    uint64_t flags;

    spin_lock_irqsave(&rq->lock, &flags);
    proc->state = PROCESS_READY;
    cpu_runqueue_enqueue_locked(rq, proc);
    spin_unlock_irqrestore(&rq->lock, flags);

#ifndef SCHEDULER_QUIET
    kprintf("[SCHEDULER] Added process '%s' (PID %d) to CPU %d runqueue (load: %d)\n",
            proc->name, proc->pid, target_cpu, rq->load);
#endif
}

void scheduler_remove_process(process_t* proc) {
    if (!proc || !proc->on_queue) {
        return;
    }

    uint32_t num_cpus = smp_cpu_count();

    // Search all CPU runqueues
    for (uint32_t cpu = 0; cpu < num_cpus; cpu++) {
        cpu_runqueue_t* rq = &runqueues[cpu];
        uint64_t flags;

        spin_lock_irqsave(&rq->lock, &flags);
        int removed = cpu_runqueue_remove_locked(rq, proc);
        spin_unlock_irqrestore(&rq->lock, flags);

        if (removed) {
#ifndef SCHEDULER_QUIET
            kprintf("[SCHEDULER] Removed process '%s' (PID %d) from CPU %d runqueue\n",
                    proc->name, proc->pid, cpu);
#endif
            process_unref(proc);
            return;
        }
    }
}

process_t* scheduler_pick_next(void) {
    uint32_t current_cpu = cpu_id();
    uint32_t num_cpus = smp_cpu_count();

    if (current_cpu >= num_cpus) {
        current_cpu = 0;
    }

    cpu_runqueue_t* rq = &runqueues[current_cpu];
    uint64_t flags;

    spin_lock_irqsave(&rq->lock, &flags);

    // Drain terminated processes
    while (rq->queue_head != NULL && rq->queue_head->state == PROCESS_TERMINATED) {
        process_t* dead = cpu_runqueue_dequeue_locked(rq);
        spin_unlock_irqrestore(&rq->lock, flags);
#ifndef SCHEDULER_QUIET
        kprintf("[SCHEDULER] pick_next: discarding terminated '%s' (PID %d)\n",
                dead->name, dead->pid);
#endif
        process_unref(dead);
        spin_lock_irqsave(&rq->lock, &flags);
    }

    // Try to get process from local runqueue
    process_t* next = cpu_runqueue_dequeue_locked(rq);
    spin_unlock_irqrestore(&rq->lock, flags);

    // If local queue empty, try work stealing
    if (!next) {
        next = work_steal(current_cpu);
    }

    // Reset time slice if needed
    if (next) {
        if (next->time_slice == 0) {
            next->time_slice = DEFAULT_TIME_SLICE;
        }
    }

    return next;
}

// Scheduler tick - called from timer interrupt
void scheduler_tick(void) {
    spin_lock(&tick_lock);
    global_tick_count++;
    uint64_t tick = global_tick_count;
    spin_unlock(&tick_lock);

    // Perform load balancing periodically
    if (tick % LOAD_BALANCE_INTERVAL == 0) {
        load_balance();
    }
}

// Get load distribution statistics
void scheduler_get_load_stats(uint32_t* loads, uint32_t max_cpus) {
    uint32_t num_cpus = smp_cpu_count();
    if (num_cpus > max_cpus) {
        num_cpus = max_cpus;
    }

    for (uint32_t cpu = 0; cpu < num_cpus; cpu++) {
        loads[cpu] = runqueues[cpu].load;
    }
}

// Print load balancing statistics
void scheduler_print_stats(void) {
    uint32_t num_cpus = smp_cpu_count();

    kprintf("\n[SCHEDULER] Load Balancing Statistics:\n");
    kprintf("  Total ticks: %llu\n", global_tick_count);
    kprintf("  Total migrations: %llu\n", total_migrations);
    kprintf("  Total work steals: %llu\n", total_steals);
    kprintf("\n  Per-CPU Load:\n");

    for (uint32_t cpu = 0; cpu < num_cpus; cpu++) {
        kprintf("    CPU %d: %d processes\n", cpu, runqueues[cpu].load);
    }
    kprintf("\n");
}

#endif // CONFIG_SMP
