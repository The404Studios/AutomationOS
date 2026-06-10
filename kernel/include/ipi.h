#ifndef IPI_H
#define IPI_H

#include "types.h"
#include "smp.h"

/*
 * Inter-Processor Interrupts (IPI)
 * =================================
 *
 * IPIs allow CPUs to send interrupts to other CPUs for:
 * - TLB shootdown (synchronize TLB flushes)
 * - Remote function calls
 * - CPU stop/halt
 * - Scheduler reschedule requests
 */

// IPI function call structure
// NOTE: When wait=true, this lives on sender's stack (blocked until completion).
//       When wait=false, caller MUST ensure this outlives all target IPIs (heap/static).
typedef struct ipi_call {
    void (*func)(void* data);           // Function to call
    void* data;                         // Argument
    uint32_t done_count;                // Number of CPUs that finished (atomic)
    uint32_t ack_count;                 // Number of CPUs that acknowledged (atomic)
    cpumask_t target_mask;              // Target CPUs
    bool wait;                          // Wait for completion?
} ipi_call_t;

// Bounded per-CPU array size for the IPI subsystem (SMP-G0). smp.h's MAX_CPUS
// is 256, which would put ~120 KB of IPI queues/stats into the packed .bss --
// pure pressure on the < 0x200000 user-shadow boundary (linker.ld: user ELFs
// link at 0x200000 and shadow any kernel global above it under their CR3; the
// IPI queues are touched from IPI handlers that run under ARBITRARY CR3, so
// they MUST stay in the packed low .bss). 8 matches the scheduler's own local
// MAX_CPUS; the live machine model is 2 (BSP + AP1).
#define IPI_MAX_CPUS 8

// IPI initialization
void ipi_init(void);

// SMP-G0 IPI-LINK acceptance: BSP sends one IPI_RESCHEDULE to CPU1, CPU1's
// handler increments its counter, BSP bounded-polls and prints
// "IPILINK: PASS ipi_resched=1 cpu1_count=1" (or FAIL). Call on the BSP only,
// after CPU1 is online and taking interrupts (post Brick E/F2).
void ipi_link_selftest(void);

// Send IPI to specific CPU
void ipi_send(uint32_t cpu, uint32_t vector);

// Send IPI to multiple CPUs
void ipi_send_mask(cpumask_t mask, uint32_t vector);

// Send IPI to all CPUs except self
void ipi_send_all_but_self(uint32_t vector);

// Send IPI to all CPUs including self
void ipi_send_all(uint32_t vector);

// TLB shootdown
void ipi_tlb_flush_all(void);
void ipi_tlb_flush_mm(void* mm);
void ipi_tlb_flush_page(void* addr);

// Remote function call
typedef void (*ipi_func_t)(void* data);

int ipi_call_function(uint32_t cpu, ipi_func_t func, void* data, bool wait);
int ipi_call_function_many(cpumask_t mask, ipi_func_t func, void* data, bool wait);
int ipi_call_function_all(ipi_func_t func, void* data, bool wait);

// Reschedule request
void ipi_reschedule(uint32_t cpu);

// Stop CPU
void ipi_stop_cpu(uint32_t cpu);
void ipi_stop_all_cpus(void);

// IPI handlers (called from interrupt handlers)
void ipi_handle_reschedule(void);
void ipi_handle_tlb_flush(void);
void ipi_handle_function_call(void);
void ipi_handle_stop(void);

// IPI statistics
typedef struct {
    uint64_t reschedule_sent;
    uint64_t reschedule_received;
    uint64_t tlb_flush_sent;
    uint64_t tlb_flush_received;
    uint64_t function_call_sent;
    uint64_t function_call_received;
    uint64_t stop_sent;
    uint64_t stop_received;
} ipi_stats_t;

extern ipi_stats_t ipi_stats[IPI_MAX_CPUS];

// SMP-G1: per-CPU wake flag (set by the IPI_RESCHEDULE handler on the target
// CPU; consumed by that CPU's idle loop inside its cli'd check window).
extern volatile uint32_t ipi_need_resched[IPI_MAX_CPUS];
uint32_t ipi_consume_need_resched(void);   // call with interrupts DISABLED

void ipi_print_stats(void);

#endif // IPI_H
