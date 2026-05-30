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
typedef struct ipi_call {
    void (*func)(void* data);           // Function to call
    void* data;                         // Argument
    volatile uint32_t done_count;       // Number of CPUs that finished
    volatile uint32_t ack_count;        // Number of CPUs that acknowledged
    cpumask_t target_mask;              // Target CPUs
    bool wait;                          // Wait for completion?
} ipi_call_t;

// IPI initialization
void ipi_init(void);

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

extern ipi_stats_t ipi_stats[MAX_CPUS];

void ipi_print_stats(void);

#endif // IPI_H
