/*
 * Inter-Processor Interrupts (IPI) Implementation
 * ================================================
 *
 * IPIs enable CPUs to communicate and synchronize:
 * - TLB shootdown for memory management coherency
 * - Remote function calls
 * - Reschedule requests
 * - CPU stop/halt
 */

#include "../../include/ipi.h"
#include "../../include/lapic.h"
#include "../../include/smp.h"
#include "../../include/kernel.h"
#include "../../include/x86_64.h"
#include "../../include/spinlock.h"
#include "../../include/tlb.h"

// IPI statistics
ipi_stats_t ipi_stats[MAX_CPUS];

// Function call queue (per-CPU)
// Stores POINTERS to caller's ipi_call_t (caller must ensure lifetime).
// Queue indices are protected by call_queue_lock; the lock is the ordering authority.
#define IPI_CALL_QUEUE_SIZE 16
#define IPI_WAIT_TIMEOUT_MS 5000  // 5 seconds timeout for IPI completion
static ipi_call_t* call_queue[MAX_CPUS][IPI_CALL_QUEUE_SIZE];
static uint32_t call_queue_head[MAX_CPUS];  // Protected by call_queue_lock
static uint32_t call_queue_tail[MAX_CPUS];  // Protected by call_queue_lock
static spinlock_t call_queue_lock[MAX_CPUS];

// TLB flush state
static volatile uint32_t tlb_flush_all_count = 0;
static volatile uint32_t tlb_flush_ack_count = 0;
static spinlock_t tlb_flush_lock;

// Initialize IPI subsystem
void ipi_init(void) {
    kprintf("[IPI] Initializing inter-processor interrupts...\n");

    // Initialize call queues
    for (uint32_t cpu = 0; cpu < MAX_CPUS; cpu++) {
        call_queue_head[cpu] = 0;
        call_queue_tail[cpu] = 0;
        spin_lock_init(&call_queue_lock[cpu]);
    }

    spin_lock_init(&tlb_flush_lock);

    // Register IPI handlers with IDT
    // Forward declarations for ASM handlers and IDT gate setter
    extern void ipi_reschedule_handler(void);
    extern void ipi_tlb_flush_handler(void);
    extern void ipi_function_call_handler(void);
    extern void ipi_stop_handler(void);
    extern void ipi_tlb_flush_all_handler(void);
    extern void idt_set_gate(uint8_t num, uint64_t handler, uint16_t selector, uint8_t flags);

    // IDT_GATE_INTERRUPT = 0x8E (present, ring 0, 64-bit interrupt gate)
    idt_set_gate(IPI_RESCHEDULE, (uint64_t)ipi_reschedule_handler, 0x08, 0x8E);
    idt_set_gate(IPI_TLB_FLUSH, (uint64_t)ipi_tlb_flush_handler, 0x08, 0x8E);
    idt_set_gate(IPI_FUNCTION_CALL, (uint64_t)ipi_function_call_handler, 0x08, 0x8E);
    idt_set_gate(IPI_STOP, (uint64_t)ipi_stop_handler, 0x08, 0x8E);
    idt_set_gate(IPI_TLB_FLUSH_ALL, (uint64_t)ipi_tlb_flush_all_handler, 0x08, 0x8E);

    kprintf("[IPI] IPI handlers registered in IDT (vectors 0x%x-0x%x)\n",
            IPI_RESCHEDULE, IPI_TLB_FLUSH_ALL);
    kprintf("[IPI] IPI subsystem initialized\n");
}

// Convert CPU ID to APIC ID
static uint32_t cpu_to_apic_id(uint32_t cpu) {
    if (cpu >= __atomic_load_n(&smp_num_cpus, __ATOMIC_ACQUIRE)) {
        return 0xFF;  // Invalid
    }
    return percpu_data[cpu].apic_id;
}

// Send IPI to specific CPU
void ipi_send(uint32_t cpu, uint32_t vector) {
    if (cpu >= __atomic_load_n(&smp_num_cpus, __ATOMIC_ACQUIRE)) {
        kprintf("[IPI] Invalid CPU: %u\n", cpu);
        return;
    }

    uint32_t apic_id = cpu_to_apic_id(cpu);
    lapic_send_ipi(apic_id, vector);
}

// Send IPI to multiple CPUs
void ipi_send_mask(cpumask_t mask, uint32_t vector) {
    uint32_t ncpus = __atomic_load_n(&smp_num_cpus, __ATOMIC_ACQUIRE);
    for (uint32_t cpu = 0; cpu < ncpus; cpu++) {
        if (cpumask_test(mask, cpu)) {
            ipi_send(cpu, vector);
        }
    }
}

// Send IPI to all CPUs except self
void ipi_send_all_but_self(uint32_t vector) {
    lapic_send_ipi_all_but_self(vector);
}

// Send IPI to all CPUs including self
void ipi_send_all(uint32_t vector) {
    lapic_send_ipi_all(vector);
}

// TLB flush all CPUs
void ipi_tlb_flush_all(void) {
    if (__atomic_load_n(&smp_num_cpus, __ATOMIC_ACQUIRE) <= 1) {
        // Single CPU, just flush local TLB
        __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
        return;
    }

    uint32_t cpu = cpu_id();
    ipi_stats[cpu].tlb_flush_sent++;

    spin_lock(&tlb_flush_lock);

    // Reset acknowledgment counter
    tlb_flush_ack_count = 0;
    tlb_flush_all_count = __atomic_load_n(&smp_num_online, __ATOMIC_ACQUIRE) - 1;  // All CPUs except self

    // Flush local TLB
    __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");

    // Send IPI to all other CPUs
    ipi_send_all_but_self(IPI_TLB_FLUSH);

    // Wait for all CPUs to acknowledge
    while (__atomic_load_n(&tlb_flush_ack_count, __ATOMIC_ACQUIRE) <
           tlb_flush_all_count) {
        __asm__ volatile("pause");
    }

    spin_unlock(&tlb_flush_lock);
}

// TLB flush for specific memory map (not yet implemented)
void ipi_tlb_flush_mm(void* mm) {
    // TODO: Implement per-mm TLB flush
    ipi_tlb_flush_all();
}

// TLB flush for specific page
void ipi_tlb_flush_page(void* addr) {
    if (__atomic_load_n(&smp_num_cpus, __ATOMIC_ACQUIRE) <= 1) {
        // Single CPU, just flush local TLB entry
        __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
        return;
    }

    // For now, flush all TLB entries (optimize later)
    ipi_tlb_flush_all();
}

// Enqueue function call
static int enqueue_call(uint32_t cpu, ipi_call_t* call) {
    spin_lock(&call_queue_lock[cpu]);

    uint32_t next = (call_queue_tail[cpu] + 1) % IPI_CALL_QUEUE_SIZE;
    if (next == call_queue_head[cpu]) {
        // Queue full
        spin_unlock(&call_queue_lock[cpu]);
        return -1;
    }

    call_queue[cpu][call_queue_tail[cpu]] = call;  // Store pointer, not value
    call_queue_tail[cpu] = next;

    spin_unlock(&call_queue_lock[cpu]);
    return 0;
}

// Dequeue function call (returns POINTER to call, not a copy)
static bool dequeue_call(uint32_t cpu, ipi_call_t** call) {
    spin_lock(&call_queue_lock[cpu]);

    if (call_queue_head[cpu] == call_queue_tail[cpu]) {
        // Queue empty
        spin_unlock(&call_queue_lock[cpu]);
        return false;
    }

    *call = call_queue[cpu][call_queue_head[cpu]];  // Return pointer
    call_queue_head[cpu] = (call_queue_head[cpu] + 1) % IPI_CALL_QUEUE_SIZE;

    spin_unlock(&call_queue_lock[cpu]);
    return true;
}

// Call function on specific CPU
int ipi_call_function(uint32_t cpu, ipi_func_t func, void* data, bool wait) {
    if (cpu >= __atomic_load_n(&smp_num_cpus, __ATOMIC_ACQUIRE)) {
        return -1;
    }

    if (cpu == cpu_id()) {
        // Same CPU, just call directly
        func(data);
        return 0;
    }

    ipi_call_t call;
    call.func = func;
    call.data = data;
    call.wait = wait;
    call.done_count = 0;
    call.ack_count = 0;
    call.target_mask = CPUMASK_CPU(cpu);

    if (enqueue_call(cpu, &call) != 0) {
        kprintf("[IPI] Function call queue full for CPU %u\n", cpu);
        return -1;
    }

    // Send IPI
    ipi_send(cpu, IPI_FUNCTION_CALL);

    uint32_t my_cpu = cpu_id();
    ipi_stats[my_cpu].function_call_sent++;

    // Wait for completion if requested (with timeout to prevent infinite hang)
    if (wait) {
        uint64_t timeout = IPI_WAIT_TIMEOUT_MS * 1000000;  // Convert to iterations
        uint64_t iterations = 0;

        while (__atomic_load_n(&call.done_count, __ATOMIC_ACQUIRE) == 0) {
            __asm__ volatile("pause");
            if (++iterations > timeout) {
                kprintf("[IPI] WARNING: IPI to CPU %u timed out after %u ms\n",
                        cpu, IPI_WAIT_TIMEOUT_MS);
                return -1;
            }
        }
    }

    return 0;
}

// Call function on multiple CPUs
int ipi_call_function_many(cpumask_t mask, ipi_func_t func, void* data, bool wait) {
    if (mask == CPUMASK_NONE) {
        return 0;
    }

    ipi_call_t call;
    call.func = func;
    call.data = data;
    call.wait = wait;
    call.done_count = 0;
    call.ack_count = 0;
    call.target_mask = mask;

    uint32_t target_count = 0;

    // Enqueue on all target CPUs
    uint32_t ncpus = __atomic_load_n(&smp_num_cpus, __ATOMIC_ACQUIRE);
    for (uint32_t cpu = 0; cpu < ncpus; cpu++) {
        if (cpumask_test(mask, cpu)) {
            if (cpu == cpu_id()) {
                // Same CPU, call directly
                func(data);
            } else {
                if (enqueue_call(cpu, &call) == 0) {
                    target_count++;
                }
            }
        }
    }

    // Send IPIs
    if (target_count > 0) {
        ipi_send_mask(mask, IPI_FUNCTION_CALL);
    }

    // Wait for completion if requested (with timeout)
    if (wait && target_count > 0) {
        uint64_t timeout = IPI_WAIT_TIMEOUT_MS * 1000000;
        uint64_t iterations = 0;

        while (__atomic_load_n(&call.done_count, __ATOMIC_ACQUIRE) < target_count) {
            __asm__ volatile("pause");
            if (++iterations > timeout) {
                uint32_t completed = __atomic_load_n(&call.done_count, __ATOMIC_ACQUIRE);
                kprintf("[IPI] WARNING: IPI broadcast timed out after %u ms "
                        "(%u/%u CPUs responded)\n",
                        IPI_WAIT_TIMEOUT_MS, completed, target_count);
                return -1;
            }
        }
    }

    return 0;
}

// Call function on all CPUs
int ipi_call_function_all(ipi_func_t func, void* data, bool wait) {
    cpumask_t mask = CPUMASK_NONE;

    uint32_t ncpus = __atomic_load_n(&smp_num_cpus, __ATOMIC_ACQUIRE);
    for (uint32_t cpu = 0; cpu < ncpus; cpu++) {
        if (cpu_is_online(cpu)) {
            cpumask_set(&mask, cpu);
        }
    }

    return ipi_call_function_many(mask, func, data, wait);
}

// Request reschedule on specific CPU
void ipi_reschedule(uint32_t cpu) {
    if (cpu >= __atomic_load_n(&smp_num_cpus, __ATOMIC_ACQUIRE) || cpu == cpu_id()) {
        return;
    }

    ipi_send(cpu, IPI_RESCHEDULE);

    uint32_t my_cpu = cpu_id();
    ipi_stats[my_cpu].reschedule_sent++;
}

// Stop specific CPU
void ipi_stop_cpu(uint32_t cpu) {
    if (cpu >= __atomic_load_n(&smp_num_cpus, __ATOMIC_ACQUIRE) || cpu == cpu_id()) {
        return;
    }

    ipi_send(cpu, IPI_STOP);

    uint32_t my_cpu = cpu_id();
    ipi_stats[my_cpu].stop_sent++;
}

// Stop all CPUs except self
void ipi_stop_all_cpus(void) {
    ipi_send_all_but_self(IPI_STOP);
}

// IPI handler: Reschedule
void ipi_handle_reschedule(void) {
    uint32_t cpu = cpu_id();
    ipi_stats[cpu].reschedule_received++;

    // Send EOI
    lapic_eoi();

    // TODO: Call scheduler to reschedule
    // schedule();
}

// IPI handler: TLB flush (redirects to lazy TLB subsystem)
void ipi_handle_tlb_flush(void) {
    uint32_t cpu = cpu_id();
    ipi_stats[cpu].tlb_flush_received++;

    // Use lazy TLB flush handler (flushes pending TLB entries)
    tlb_handle_ipi_flush();

    // Acknowledge (for old-style synchronous flush API compatibility)
    __atomic_add_fetch(&tlb_flush_ack_count, 1, __ATOMIC_RELEASE);

    // Send EOI
    lapic_eoi();
}

// IPI handler: TLB flush all contexts (PCID recycle)
void ipi_handle_tlb_flush_all(void) {
    uint32_t cpu = cpu_id();
    ipi_stats[cpu].tlb_flush_received++;

    // Flush ALL PCIDs unconditionally (bypass lazy state)
    tlb_handle_ipi_flush_all_contexts();

    // Acknowledge for synchronous wait
    __atomic_add_fetch(&tlb_flush_ack_count, 1, __ATOMIC_RELEASE);

    // Send EOI
    lapic_eoi();
}

// IPI handler: Function call
void ipi_handle_function_call(void) {
    uint32_t cpu = cpu_id();
    ipi_stats[cpu].function_call_received++;

    // Process all pending function calls
    ipi_call_t* call;
    while (dequeue_call(cpu, &call)) {
        // Execute function
        call->func(call->data);

        // Mark as done on the SHARED object (not a throwaway copy)
        __atomic_add_fetch(&call->done_count, 1, __ATOMIC_RELEASE);
    }

    // Send EOI
    lapic_eoi();
}

// IPI handler: Stop CPU
void ipi_handle_stop(void) {
    uint32_t cpu = cpu_id();
    ipi_stats[cpu].stop_received++;

    kprintf("[IPI] CPU %u received STOP IPI\n", cpu);

    // Send EOI
    lapic_eoi();

    // Disable interrupts and halt
    cli();
    while (1) {
        hlt();
    }
}

// Print IPI statistics
void ipi_print_stats(void) {
    kprintf("\n=== IPI Statistics ===\n");

    uint32_t ncpus = __atomic_load_n(&smp_num_cpus, __ATOMIC_ACQUIRE);
    for (uint32_t cpu = 0; cpu < ncpus; cpu++) {
        if (!cpu_is_online(cpu)) continue;

        ipi_stats_t* stats = &ipi_stats[cpu];

        kprintf("CPU %u:\n", cpu);
        kprintf("  Reschedule: sent=%llu, received=%llu\n",
                stats->reschedule_sent, stats->reschedule_received);
        kprintf("  TLB flush: sent=%llu, received=%llu\n",
                stats->tlb_flush_sent, stats->tlb_flush_received);
        kprintf("  Function call: sent=%llu, received=%llu\n",
                stats->function_call_sent, stats->function_call_received);
        kprintf("  Stop: sent=%llu, received=%llu\n",
                stats->stop_sent, stats->stop_received);
        kprintf("\n");
    }
}
