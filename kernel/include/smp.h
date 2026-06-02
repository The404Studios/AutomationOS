#ifndef SMP_H
#define SMP_H

#include "types.h"
#include "spinlock.h"

/*
 * SMP (Symmetric Multiprocessing) Support
 * ========================================
 *
 * Complete multi-core CPU support for AutomationOS
 * - CPU detection and initialization via ACPI MADT
 * - AP (Application Processor) startup via INIT-SIPI-SIPI
 * - Per-CPU data structures
 * - Inter-Processor Interrupts (IPI)
 * - SMP-safe scheduling with per-CPU run queues
 */

// Maximum number of CPUs supported
#define MAX_CPUS 256

// CPU states
typedef enum {
    CPU_STATE_OFFLINE = 0,
    CPU_STATE_ONLINE,
    CPU_STATE_STARTING,
    CPU_STATE_STOPPING,
    CPU_STATE_FAILED
} cpu_state_t;

// CPU flags
#define CPU_FLAG_BSP        0x0001  // Bootstrap Processor
#define CPU_FLAG_ENABLED    0x0002  // CPU is enabled
#define CPU_FLAG_HOTPLUG    0x0004  // Supports hotplug
#define CPU_FLAG_HT         0x0008  // Hyper-Threading enabled

// Forward declarations
struct process;
struct runqueue;

// Per-CPU data structure (one per CPU)
typedef struct {
    uint32_t cpu_id;                    // Logical CPU ID (0, 1, 2, ...)
    uint32_t apic_id;                   // Local APIC ID
    cpu_state_t state;                  // Current state
    uint32_t flags;                     // CPU flags

    // Current execution context
    struct process* current_thread;     // Currently running thread
    struct process* current_process;    // Currently running process
    struct process* idle_thread;        // Idle thread for this CPU

    // Scheduler data (per-CPU run queue)
    struct runqueue* runqueue;          // This CPU's run queue

    // Statistics
    uint64_t total_ticks;               // Total clock ticks
    uint64_t idle_ticks;                // Ticks spent idle
    uint64_t user_ticks;                // Ticks spent in user mode
    uint64_t kernel_ticks;              // Ticks spent in kernel mode

    // Interrupt handling
    uint64_t interrupt_count;           // Total interrupts serviced
    bool interrupts_enabled;            // IRQs enabled?
    uint32_t preempt_count;             // Preemption disabled when > 0
    uint64_t irq_stack_ptr;             // IRQ stack pointer

    // Stacks
    void* kernel_stack;                 // Kernel stack
    void* interrupt_stack;              // Interrupt stack

    // TLB shootdown
    volatile bool tlb_flush_pending;    // TLB flush requested

    // Cache (for PMM)
    void* page_cache_pages[16];         // Cached free pages
    uint32_t page_cache_count;          // Number of cached pages
    uint64_t page_cache_hits;           // Cache hit count
    uint64_t page_cache_misses;         // Cache miss count
    spinlock_t page_cache_lock;         // Cache lock

    // Health monitoring (leak detection + stall detection)
    struct {
        uint32_t ownership_allocs;      // Total ownership allocations
        uint32_t ownership_frees;       // Total ownership frees
        uint32_t ownership_leaks;       // Detected leaks (allocs - frees)
        volatile uint64_t heartbeat;    // Incremented by timer tick (for stall detection)
        uint64_t last_heartbeat;        // Previous heartbeat value (for stall detection)
    } health;
} percpu_data_t;

// CPU information structure
typedef struct {
    uint32_t cpu_id;                    // Logical CPU ID
    uint32_t apic_id;                   // Local APIC ID
    uint32_t flags;                     // CPU flags
    cpu_state_t state;                  // Current state

    // CPU identification
    char vendor[13];                    // CPU vendor string
    char brand[49];                     // CPU brand string
    uint32_t family;                    // CPU family
    uint32_t model;                     // CPU model
    uint32_t stepping;                  // CPU stepping

    // Capabilities
    bool has_apic;                      // Has Local APIC
    bool has_x2apic;                    // Has x2APIC
    bool has_tsc;                       // Has Time Stamp Counter
    bool has_msr;                       // Has Model Specific Registers
    bool has_sse;                       // Has SSE
    bool has_sse2;                      // Has SSE2
    bool has_avx;                       // Has AVX
    bool has_avx2;                      // Has AVX2
} cpu_info_t;

// Global SMP state
extern uint32_t smp_num_cpus;           // Number of detected CPUs
extern uint32_t smp_num_online;         // Number of online CPUs
extern percpu_data_t percpu_data[MAX_CPUS];
extern cpu_info_t cpu_info[MAX_CPUS];

// SMP initialization
// smp_init() returns the number of logical CPUs known (>= 1). With SMP_ENABLE
// off it always returns 1 (single-CPU stub); with it on it also brings APs up.
int smp_init(void);
uint32_t smp_cpu_count(void);   // total logical CPUs (1 unless APs brought up)
void smp_detect_cpus(void);
void smp_start_aps(void);

// 64-bit C entry point for an Application Processor (called from the trampoline
// in ap_trampoline.asm). Marks the AP online and parks it. Not called directly.
void smp_ap_main(uint64_t cpu);

// CPU control
int cpu_online(uint32_t cpu);
int cpu_offline(uint32_t cpu);
bool cpu_is_online(uint32_t cpu);
cpu_state_t cpu_get_state(uint32_t cpu);

// CPU ID functions
uint32_t cpu_id(void);                  // Get current CPU ID
uint32_t cpu_count(void);               // Get total CPU count
uint32_t apic_id(void);                 // Get current APIC ID

// Per-CPU data access
percpu_data_t* this_cpu(void);          // Get current CPU data
percpu_data_t* cpu_data(uint32_t cpu);  // Get specific CPU data

// CPU iteration
#define for_each_cpu(cpu) \
    for (uint32_t cpu = 0; cpu < smp_num_cpus; cpu++)

#define for_each_online_cpu(cpu) \
    for (uint32_t cpu = 0; cpu < smp_num_cpus; cpu++) \
        if (cpu_is_online(cpu))

// Preemption control
void preempt_disable(void);
void preempt_enable(void);
bool preempt_is_disabled(void);

// CPU identification
void cpu_identify(uint32_t cpu);
const char* cpu_vendor_string(uint32_t cpu);
const char* cpu_brand_string(uint32_t cpu);

// CPU hotplug callbacks
typedef void (*cpu_online_cb_t)(uint32_t cpu);
typedef void (*cpu_offline_cb_t)(uint32_t cpu);

int register_cpu_online_callback(cpu_online_cb_t callback);
int register_cpu_offline_callback(cpu_offline_cb_t callback);

// CPU affinity mask
typedef uint64_t cpumask_t;

#define CPUMASK_NONE        0
#define CPUMASK_ALL         ((cpumask_t)-1)
#define CPUMASK_CPU(cpu)    ((cpumask_t)1 << (cpu))

static inline bool cpumask_test(cpumask_t mask, uint32_t cpu) {
    return (mask & CPUMASK_CPU(cpu)) != 0;
}

static inline void cpumask_set(cpumask_t* mask, uint32_t cpu) {
    *mask |= CPUMASK_CPU(cpu);
}

static inline void cpumask_clear(cpumask_t* mask, uint32_t cpu) {
    *mask &= ~CPUMASK_CPU(cpu);
}

static inline uint32_t cpumask_first(cpumask_t mask) {
    return __builtin_ctzll(mask);
}

static inline uint32_t cpumask_count(cpumask_t mask) {
    return __builtin_popcountll(mask);
}

// Debug and statistics
void smp_print_info(void);
void smp_print_stats(void);

// Health monitoring
bool health_monitor_detect_stalls(void);

#endif // SMP_H
