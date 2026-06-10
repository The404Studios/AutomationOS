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
#include "../../include/lapic_constants.h"   /* AP_LAPIC_TIMER_VECTOR, SPURIOUS_VECTOR */
#include "../../include/smp.h"
#include "../../include/kernel.h"
#include "../../include/x86_64.h"
#include "../../include/spinlock.h"
#include "../../include/tlb.h"
#include "../../include/perf.h"              /* rdtsc (static inline) -- the bounded selftest wait */

/* ===========================================================================
 * SMP-G0 VECTOR-COLLISION CHECKS (explicit, compile-time -- never assumptions).
 * The IDT vector landscape this kernel actually claims:
 *   0x00-0x1F  CPU exceptions          (idt.c isr0-31)
 *   0x20-0x2F  remapped PIC IRQs       (idt.c irq0-15; 0x20 = the BSP tick)
 *   0x40       CPU1 LAPIC timer        (AP_LAPIC_TIMER_VECTOR, Brick E)
 *   0xFF       LAPIC spurious          (SPURIOUS_VECTOR + ap_spurious_isr)
 * The original IPI block sat at 0x40 -- DEAD ON the AP timer gate. Any future
 * renumber that re-collides fails THIS build, not a 2 AM debug session.
 * =========================================================================== */
_Static_assert(IPI_RESCHEDULE > 0x2F && IPI_AP_PANIC < 0xFF,
               "IPI vector block must sit above exceptions+PIC and below spurious");
_Static_assert(IPI_RESCHEDULE   < IPI_TLB_FLUSH     && IPI_TLB_FLUSH     < IPI_FUNCTION_CALL &&
               IPI_FUNCTION_CALL < IPI_STOP         && IPI_STOP          < IPI_TEST &&
               IPI_TEST          < IPI_TLB_FLUSH_ALL && IPI_TLB_FLUSH_ALL < IPI_AP_PANIC,
               "IPI vectors must be strictly increasing (distinct)");
_Static_assert(AP_LAPIC_TIMER_VECTOR < IPI_RESCHEDULE || AP_LAPIC_TIMER_VECTOR > IPI_AP_PANIC,
               "IPI vector block collides with the CPU1 LAPIC timer vector");
_Static_assert(LAPIC_TIMER_VECTOR < IPI_RESCHEDULE || LAPIC_TIMER_VECTOR > IPI_AP_PANIC,
               "IPI vector block collides with the default LAPIC timer vector");
_Static_assert(SPURIOUS_VECTOR < IPI_RESCHEDULE || SPURIOUS_VECTOR > IPI_AP_PANIC,
               "IPI vector block collides with the LAPIC spurious vector");

/* ===========================================================================
 * SMP-G0 CPU-MODEL SEAM. This file was salvage written against smp.c's model
 * (smp_num_cpus / percpu_data[].apic_id / cpu_is_online) -- smp.c is NOT in
 * the build. The live tree's model (ap_boot.c) is: 2 logical CPUs, CPU1's
 * APIC id captured from the MADT in try_start_cpu1(), liveness via
 * cpu1_is_online(). Resolve everything through that seam. Critically,
 * percpu_data[] DOES link (ap_boot.c defines a minimal array for the health
 * monitor) but its .apic_id is NEVER FILLED -- the old percpu_data-based
 * cpu_to_apic_id would have returned 0 for CPU1 and sent every "CPU1" IPI to
 * the BSP ITSELF. Wrong-target, not link-fail: the worst kind.
 * =========================================================================== */
extern int      cpu1_is_online(void);       /* ap_boot.c: AP up + not offlined   */
extern uint32_t smp_cpu1_apic_id(void);     /* ap_boot.c: CPU1 hw APIC id (G0)   */

static uint32_t ipi_bsp_apic_id = 0;        /* captured in ipi_init (BSP context) */
static int      ipi_ready       = 0;        /* gates claimed + vectors verified   */

static uint32_t ipi_ncpus(void) {
    return cpu1_is_online() ? 2u : 1u;
}

static bool ipi_cpu_online(uint32_t cpu) {
    if (cpu == 0) return true;
    if (cpu == 1) return cpu1_is_online() != 0;
    return false;
}

// IPI statistics
ipi_stats_t ipi_stats[IPI_MAX_CPUS];

// Function call queue (per-CPU)
// Stores call requests BY VALUE so they do NOT depend on the sender's stack
// lifetime. This is what makes wait==false (fire-and-forget) safe: the sender
// may return immediately and reclaim its stack frame while the target CPU is
// still draining the queue.
//   - func/data are copied into the queue and owned by it.
//   - done_ptr points at the sender's done_count and is non-NULL ONLY when the
//     sender is blocking in wait==true (and is therefore guaranteed alive until
//     done_count is signalled). For wait==false there is no waiter, so done_ptr
//     is NULL and the target performs no write-back to sender memory.
// Queue indices are protected by call_queue_lock; the lock is the ordering authority.
#define IPI_CALL_QUEUE_SIZE 16
#define IPI_WAIT_TIMEOUT_MS 5000  // 5 seconds timeout for IPI completion
typedef struct ipi_call_entry {
    ipi_func_t func;       // Function to call
    void*      data;       // Argument
    uint32_t*  done_ptr;   // &sender->done_count (wait==true) or NULL (wait==false)
} ipi_call_entry_t;
static ipi_call_entry_t call_queue[IPI_MAX_CPUS][IPI_CALL_QUEUE_SIZE];
static uint32_t call_queue_head[IPI_MAX_CPUS];  // Protected by call_queue_lock
static uint32_t call_queue_tail[IPI_MAX_CPUS];  // Protected by call_queue_lock
static spinlock_t call_queue_lock[IPI_MAX_CPUS];

// TLB flush state
static volatile uint32_t tlb_flush_all_count = 0;
static volatile uint32_t tlb_flush_ack_count = 0;
static spinlock_t tlb_flush_lock;

// Initialize IPI subsystem. BSP context, after lapic_init(), BEFORE
// try_start_cpu1() -- the IDT is shared, so the gates must exist before CPU1
// is alive enough to ever receive one of these vectors.
void ipi_init(void) {
    kprintf("[IPI] Initializing inter-processor interrupts...\n");

    // Initialize call queues
    for (uint32_t cpu = 0; cpu < IPI_MAX_CPUS; cpu++) {
        call_queue_head[cpu] = 0;
        call_queue_tail[cpu] = 0;
        spin_lock_init(&call_queue_lock[cpu]);
    }

    spin_lock_init(&tlb_flush_lock);

    // We run on the BSP here: capture its hardware APIC id for cpu_to_apic_id.
    ipi_bsp_apic_id = lapic_get_id();

    // Register IPI handlers with IDT
    // Forward declarations for ASM handlers and IDT gate setter
    extern void ipi_reschedule_handler(void);
    extern void ipi_tlb_flush_handler(void);
    extern void ipi_function_call_handler(void);
    extern void ipi_stop_handler(void);
    extern void ipi_tlb_flush_all_handler(void);
    extern void idt_set_gate(uint8_t num, uint64_t handler, uint16_t selector, uint8_t flags);
    extern int  idt_gate_present(uint8_t num);

    // RUNTIME collision check (the compile-time asserts above cover the KNOWN
    // claimants; this catches any future runtime registration we don't know
    // about). Refuse to claim an occupied gate -- a silent overwrite is how
    // the original 0x40 block would have killed CPU1's timer.
    static const uint8_t ipi_vectors[] = {
        IPI_RESCHEDULE, IPI_TLB_FLUSH, IPI_FUNCTION_CALL,
        IPI_STOP, IPI_TLB_FLUSH_ALL
    };
    for (uint32_t i = 0; i < sizeof(ipi_vectors); i++) {
        if (idt_gate_present(ipi_vectors[i])) {
            kprintf("[IPI] FATAL: IDT vector 0x%x already claimed -- IPI "
                    "subsystem NOT initialized (collision)\n", ipi_vectors[i]);
            return;                       /* ipi_ready stays 0; senders no-op */
        }
    }

    // IDT_GATE_INTERRUPT = 0x8E (present, ring 0, 64-bit interrupt gate)
    idt_set_gate(IPI_RESCHEDULE, (uint64_t)ipi_reschedule_handler, 0x08, 0x8E);
    idt_set_gate(IPI_TLB_FLUSH, (uint64_t)ipi_tlb_flush_handler, 0x08, 0x8E);
    idt_set_gate(IPI_FUNCTION_CALL, (uint64_t)ipi_function_call_handler, 0x08, 0x8E);
    idt_set_gate(IPI_STOP, (uint64_t)ipi_stop_handler, 0x08, 0x8E);
    idt_set_gate(IPI_TLB_FLUSH_ALL, (uint64_t)ipi_tlb_flush_all_handler, 0x08, 0x8E);

    ipi_ready = 1;
    kprintf("[IPI] IPI handlers registered in IDT (vectors 0x%x-0x%x, bsp_apic=%u)\n",
            IPI_RESCHEDULE, IPI_TLB_FLUSH_ALL, ipi_bsp_apic_id);
    kprintf("[IPI] IPI subsystem initialized\n");
}

// Convert CPU ID to APIC ID via the live ap_boot.c seam (NOT percpu_data --
// see the seam note at the top of this file).
static uint32_t cpu_to_apic_id(uint32_t cpu) {
    if (cpu == 0) return ipi_bsp_apic_id;
    if (cpu == 1) return smp_cpu1_apic_id();   /* 0xFFFFFFFF until captured */
    return 0xFFFFFFFFu;  // Invalid
}

// Send IPI to specific CPU
void ipi_send(uint32_t cpu, uint32_t vector) {
    if (!ipi_ready) {
        return;       /* gates never claimed (collision/init skipped) -- no-op */
    }
    if (cpu >= ipi_ncpus()) {
        kprintf("[IPI] Invalid CPU: %u\n", cpu);
        return;
    }

    uint32_t apic_id = cpu_to_apic_id(cpu);
    if (apic_id == 0xFFFFFFFFu) {
        kprintf("[IPI] CPU %u has no captured APIC id -- IPI dropped\n", cpu);
        return;
    }
    lapic_send_ipi(apic_id, vector);
}

// Send IPI to multiple CPUs
void ipi_send_mask(cpumask_t mask, uint32_t vector) {
    uint32_t ncpus = ipi_ncpus();
    for (uint32_t cpu = 0; cpu < ncpus; cpu++) {
        if (cpumask_test(mask, cpu)) {
            ipi_send(cpu, vector);
        }
    }
}

// Send IPI to all CPUs except self
void ipi_send_all_but_self(uint32_t vector) {
    if (!ipi_ready) {
        return;       /* no gates claimed -> a broadcast would #GP the targets */
    }
    lapic_send_ipi_all_but_self(vector);
}

// Send IPI to all CPUs including self
void ipi_send_all(uint32_t vector) {
    if (!ipi_ready) {
        return;
    }
    lapic_send_ipi_all(vector);
}

// TLB flush all CPUs
void ipi_tlb_flush_all(void) {
    if (ipi_ncpus() <= 1) {
        // Single CPU, just flush local TLB
        __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
        return;
    }

    uint32_t cpu = cpu_id();
    ipi_stats[cpu].tlb_flush_sent++;

    spin_lock(&tlb_flush_lock);

    // Reset acknowledgment counter
    tlb_flush_ack_count = 0;
    tlb_flush_all_count = ipi_ncpus() - 1;  // All CPUs except self

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
    if (ipi_ncpus() <= 1) {
        // Single CPU, just flush local TLB entry
        __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
        return;
    }

    // For now, flush all TLB entries (optimize later)
    ipi_tlb_flush_all();
}

// Enqueue function call (copies the request BY VALUE into the queue)
static int enqueue_call(uint32_t cpu, ipi_func_t func, void* data, uint32_t* done_ptr) {
    spin_lock(&call_queue_lock[cpu]);

    uint32_t next = (call_queue_tail[cpu] + 1) % IPI_CALL_QUEUE_SIZE;
    if (next == call_queue_head[cpu]) {
        // Queue full
        spin_unlock(&call_queue_lock[cpu]);
        return -1;
    }

    ipi_call_entry_t* slot = &call_queue[cpu][call_queue_tail[cpu]];
    slot->func = func;          // Store value, not a pointer into caller stack
    slot->data = data;
    slot->done_ptr = done_ptr;
    call_queue_tail[cpu] = next;

    spin_unlock(&call_queue_lock[cpu]);
    return 0;
}

// Dequeue function call (copies the entry out BY VALUE)
static bool dequeue_call(uint32_t cpu, ipi_call_entry_t* out) {
    spin_lock(&call_queue_lock[cpu]);

    if (call_queue_head[cpu] == call_queue_tail[cpu]) {
        // Queue empty
        spin_unlock(&call_queue_lock[cpu]);
        return false;
    }

    *out = call_queue[cpu][call_queue_head[cpu]];  // Copy out by value
    call_queue_head[cpu] = (call_queue_head[cpu] + 1) % IPI_CALL_QUEUE_SIZE;

    spin_unlock(&call_queue_lock[cpu]);
    return true;
}

// Call function on specific CPU
int ipi_call_function(uint32_t cpu, ipi_func_t func, void* data, bool wait) {
    if (cpu >= ipi_ncpus()) {
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

    // Only pass a write-back pointer when the sender will block (wait==true) and
    // is therefore guaranteed alive until done_count is signalled. For wait==false
    // the queued copy owns everything and the target writes nothing back into this
    // (about-to-be-reclaimed) stack frame.
    if (enqueue_call(cpu, func, data, wait ? &call.done_count : NULL) != 0) {
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

    // Only hand the target a write-back pointer when the sender will block
    // (wait==true) and thus outlives the targets. For wait==false the queued
    // copies are self-contained and nothing is written back to this stack frame.
    uint32_t* done_ptr = wait ? &call.done_count : NULL;

    // Enqueue on all target CPUs
    uint32_t ncpus = ipi_ncpus();
    for (uint32_t cpu = 0; cpu < ncpus; cpu++) {
        if (cpumask_test(mask, cpu)) {
            if (cpu == cpu_id()) {
                // Same CPU, call directly
                func(data);
            } else {
                if (enqueue_call(cpu, func, data, done_ptr) == 0) {
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

    uint32_t ncpus = ipi_ncpus();
    for (uint32_t cpu = 0; cpu < ncpus; cpu++) {
        if (ipi_cpu_online(cpu)) {
            cpumask_set(&mask, cpu);
        }
    }

    return ipi_call_function_many(mask, func, data, wait);
}

// Request reschedule on specific CPU
void ipi_reschedule(uint32_t cpu) {
    if (cpu >= ipi_ncpus() || cpu == cpu_id()) {
        return;
    }

    ipi_send(cpu, IPI_RESCHEDULE);

    uint32_t my_cpu = cpu_id();
    ipi_stats[my_cpu].reschedule_sent++;
}

// Stop specific CPU
void ipi_stop_cpu(uint32_t cpu) {
    if (cpu >= ipi_ncpus() || cpu == cpu_id()) {
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

    // Process all pending function calls. The entry is a by-value copy, so it is
    // valid regardless of the sender's stack lifetime (fixes the wait==false
    // use-after-return).
    ipi_call_entry_t entry;
    while (dequeue_call(cpu, &entry)) {
        // Execute function
        entry.func(entry.data);

        // Signal completion only for waiting senders. done_ptr is NULL for
        // fire-and-forget (wait==false), so we never write into a reclaimed
        // sender stack frame.
        if (entry.done_ptr) {
            __atomic_add_fetch(entry.done_ptr, 1, __ATOMIC_RELEASE);
        }
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

/* ===========================================================================
 * SMP-G0 IPI-LINK acceptance: the smallest possible proof that the BSP can
 * interrupt CPU1 and CPU1 can handle it safely.
 *   BSP: ipi_reschedule(1) -> ONE IPI_RESCHEDULE
 *   CPU1: ipi_reschedule_handler (asm) -> ipi_handle_reschedule (C) ->
 *         reschedule_received++ + LAPIC EOI + iretq. NO scheduling action --
 *         wake-by-IPI is SMP-G1, explicitly out of scope here.
 * BSP context only, after CPU1 is online and taking interrupts (Brick E
 * proved the timer; F3-5 proved sti;hlt parking -- either state takes this).
 * Bounded TSC wait (~100 ms, the try_start_cpu1 convention); a dead CPU1
 * means FAIL on serial, never a hang.
 * =========================================================================== */
void ipi_link_selftest(void) {
    if (!ipi_ready) {
        kprintf("IPILINK: FAIL ipi_resched=0 cpu1_count=0 (init refused/skipped)\n");
        return;
    }
    if (!ipi_cpu_online(1)) {
        kprintf("IPILINK: SKIP (CPU1 offline -- single-core boot)\n");
        return;
    }

    uint64_t rx_before   = __atomic_load_n(&ipi_stats[1].reschedule_received, __ATOMIC_ACQUIRE);
    uint64_t sent_before = ipi_stats[0].reschedule_sent;

    ipi_reschedule(1);

    int sent = (ipi_stats[0].reschedule_sent == sent_before + 1);

    /* Bounded wait: ~100 ms TSC deadline (3 GHz estimate -- only the BOUND
     * matters, not the exact frequency). */
    uint64_t start    = rdtsc();
    uint64_t deadline = 100000ULL * 3000ULL;
    uint64_t rx_now   = rx_before;
    while ((rx_now = __atomic_load_n(&ipi_stats[1].reschedule_received,
                                     __ATOMIC_ACQUIRE)) == rx_before) {
        if ((rdtsc() - start) >= deadline) break;
        __asm__ volatile("pause" ::: "memory");
    }

    if (sent && rx_now > rx_before) {
        kprintf("IPILINK: PASS ipi_resched=1 cpu1_count=1\n");
    } else {
        kprintf("IPILINK: FAIL ipi_resched=%d cpu1_count=%lu (rx %lu -> %lu)\n",
                sent, (unsigned long)(rx_now - rx_before),
                (unsigned long)rx_before, (unsigned long)rx_now);
    }
}

// Print IPI statistics
void ipi_print_stats(void) {
    kprintf("\n=== IPI Statistics ===\n");

    uint32_t ncpus = ipi_ncpus();
    for (uint32_t cpu = 0; cpu < ncpus; cpu++) {
        if (!ipi_cpu_online(cpu)) continue;

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
