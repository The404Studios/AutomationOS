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
#include "../../include/sched.h"             /* SMP-G2: process_t walk for tlb_pinning audit */

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
_Static_assert(IPI_RESCHEDULE > 0x2F && IPI_TLB_FLUSH_PAGE < 0xFF,
               "IPI vector block must sit above exceptions+PIC and below spurious");
_Static_assert(IPI_RESCHEDULE   < IPI_TLB_FLUSH     && IPI_TLB_FLUSH     < IPI_FUNCTION_CALL &&
               IPI_FUNCTION_CALL < IPI_STOP         && IPI_STOP          < IPI_TEST &&
               IPI_TEST          < IPI_TLB_FLUSH_ALL && IPI_TLB_FLUSH_ALL < IPI_AP_PANIC &&
               IPI_AP_PANIC      < IPI_TLB_FLUSH_PAGE,
               "IPI vectors must be strictly increasing (distinct)");
_Static_assert(AP_LAPIC_TIMER_VECTOR < IPI_RESCHEDULE || AP_LAPIC_TIMER_VECTOR > IPI_TLB_FLUSH_PAGE,
               "IPI vector block collides with the CPU1 LAPIC timer vector");
_Static_assert(LAPIC_TIMER_VECTOR < IPI_RESCHEDULE || LAPIC_TIMER_VECTOR > IPI_TLB_FLUSH_PAGE,
               "IPI vector block collides with the default LAPIC timer vector");
_Static_assert(SPURIOUS_VECTOR < IPI_RESCHEDULE || SPURIOUS_VECTOR > IPI_TLB_FLUSH_PAGE,
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

/* SMP-G1: per-CPU need_resched -- the wake flag between the IPI_RESCHEDULE
 * handler (producer, on the target CPU) and that CPU's idle loop (consumer,
 * in its cli'd check window). One writer per slot per side, plain volatile
 * stores; the cli window is what makes consume atomic vs the handler. Lives
 * in ipi.c's packed low .bss (law 15: handler data under arbitrary CR3). */
volatile uint32_t ipi_need_resched[IPI_MAX_CPUS];

/* Consume THIS cpu's need_resched flag. MUST be called with interrupts
 * disabled (the idle loop's cli'd check) so the handler cannot interleave
 * between the read and the clear. Returns the pre-clear value. */
uint32_t ipi_consume_need_resched(void) {
    uint32_t cpu = cpu_id();
    uint32_t v = ipi_need_resched[cpu];
    ipi_need_resched[cpu] = 0;
    return v;
}

/* SMP-G1 proof-plumbing globals. DEFINED HERE (not scheduler.c) deliberately:
 * ipi.c's objects link into the LOW packed .bss (~0x19b000), below the strict
 * 0x200000 gate; scheduler.c's land at ~0x23c000. These fields are read from
 * CPU1 contexts that can hold a USER CR3 (the AP dying path runs
 * ap_cooperative_schedule under the dead task's address space), so they obey
 * law 15 like the rest of the IPI state. The ipiwake smoke's nm gate CAUGHT
 * this exact placement when they first lived in scheduler.c. */
volatile uint64_t g_g1_ping_req      = 0;   // BSP: rdtsc at IPI send (0 = idle)
volatile uint64_t g_g1_ping_ack      = 0;   // CPU1: rdtsc at flag consume
volatile uint64_t g_g1_enq_tsc       = 0;   // BSP: rdtsc at cpu1hello enqueue
volatile int      g_g1_enq_pid       = 0;   // BSP: the enqueued pid to match
volatile uint64_t g_g1_dispatch_tsc  = 0;   // CPU1: rdtsc at first dispatch of that pid

#ifdef SMP_BATCH
/* SMP-F3-7: the same enqueue->first-dispatch stamp pair for the batchdemo
 * placement -- proving the BATCH foreign enqueue rode the G1 IPI kick (a
 * sub-ms dispatch is IPI-woken; the tick floor is 10 ms). Same law-15 home. */
volatile uint64_t g_f37_enq_tsc      = 0;   // BSP: rdtsc at batchdemo enqueue
volatile int      g_f37_enq_pid      = 0;   // BSP: the enqueued pid to match
volatile uint64_t g_f37_dispatch_tsc = 0;   // CPU1: rdtsc at first dispatch
#endif

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

/* SMP-G2 shootdown state (the machinery itself lives lower in this file;
 * the state sits here, in the packed low .bss with the rest of the IPI
 * data, because the 0x57 handler reads it under arbitrary CR3 -- law 15). */
#define TLBSHOOT_MAX_PAGES   64        /* > this (or 0) -> full-flush fallback */
#define TLBSHOOT_WAIT_US     50000ULL  /* 50 ms TSC-bounded ack wait */
static volatile uint64_t tlbshoot_addr   = 0;   /* request block: page-aligned VA */
static volatile uint64_t tlbshoot_npages = 0;   /* request block: page count      */
static volatile uint32_t tlbshoot_ack    = 0;   /* remote handler increments      */
static spinlock_t        tlbshoot_lock;         /* dedicated in-flight serializer */
volatile uint32_t g_tlb_invariant_violations = 0;
void tlb_invariant_violation(const char* what);
#ifdef SMP_RUNMASK
int  runmask_audit_crosscpu(int* checked_out);   /* defined below the selftest */
void runmask_selftest(void);
#endif

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
    spin_lock_init(&tlbshoot_lock);          /* SMP-G2 shootdown serializer */

    // We run on the BSP here: capture its hardware APIC id for cpu_to_apic_id.
    ipi_bsp_apic_id = lapic_get_id();

    // Register IPI handlers with IDT
    // Forward declarations for ASM handlers and IDT gate setter
    extern void ipi_reschedule_handler(void);
    extern void ipi_tlb_flush_handler(void);
    extern void ipi_function_call_handler(void);
    extern void ipi_stop_handler(void);
    extern void ipi_tlb_flush_all_handler(void);
    extern void ipi_tlb_flush_page_handler(void);
    extern void idt_set_gate(uint8_t num, uint64_t handler, uint16_t selector, uint8_t flags);
    extern int  idt_gate_present(uint8_t num);

    // RUNTIME collision check (the compile-time asserts above cover the KNOWN
    // claimants; this catches any future runtime registration we don't know
    // about). Refuse to claim an occupied gate -- a silent overwrite is how
    // the original 0x40 block would have killed CPU1's timer.
    static const uint8_t ipi_vectors[] = {
        IPI_RESCHEDULE, IPI_TLB_FLUSH, IPI_FUNCTION_CALL,
        IPI_STOP, IPI_TLB_FLUSH_ALL, IPI_TLB_FLUSH_PAGE
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
    idt_set_gate(IPI_TLB_FLUSH_PAGE, (uint64_t)ipi_tlb_flush_page_handler, 0x08, 0x8E);

    ipi_ready = 1;
    kprintf("[IPI] IPI handlers registered in IDT (vectors 0x%x-0x%x, bsp_apic=%u)\n",
            IPI_RESCHEDULE, IPI_TLB_FLUSH_PAGE, ipi_bsp_apic_id);
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

    // Wait for all CPUs to acknowledge. SMP-G2 / LAW 16: the original wait
    // here was UNBOUNDED (a dead/parked CPU = permanent hang). TSC-bounded;
    // a timeout is a loud TLB_INVARIANT violation, never a hang. NOTE
    // tlb_flush_lock is the shootdown's own dedicated serializer -- holding
    // it across the wait is the design; law 16 forbids waiting under
    // SCHEDULER/RQ/HEAP/FS locks, which this path must never be called with.
    {
        extern void tlb_invariant_violation(const char* what);
        uint64_t s = rdtsc();
        while (__atomic_load_n(&tlb_flush_ack_count, __ATOMIC_ACQUIRE) <
               tlb_flush_all_count) {
            if ((rdtsc() - s) >= 50000ULL * 3000ULL) {   /* 50 ms */
                tlb_invariant_violation("ipi_tlb_flush_all ack timeout");
                break;
            }
            __asm__ volatile("pause");
        }
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

    /* SMP-G1: the handler's ONLY scheduling action is setting the per-CPU
     * wake flag. The interrupted context decides what to do with it: the AP
     * idle loop's cli'd check consumes it and skips the hlt (the lost-wakeup
     * close). NO schedule() call from interrupt context here -- dispatch
     * stays in ap_cooperative_schedule / the timer path (no rewrite, per the
     * G1 hard no's). */
    ipi_need_resched[cpu] = 1;

    // Send EOI
    lapic_eoi();
}

// IPI handler: TLB flush
void ipi_handle_tlb_flush(void) {
    uint32_t cpu = cpu_id();
    ipi_stats[cpu].tlb_flush_received++;

    // SMP-G2 FALSE-ACK FIX: the linked TLB layer is tlb_uni.c, whose
    // tlb_handle_ipi_flush() is a single-CPU NO-OP ("no IPIs on a single
    // CPU") -- the original code here would have ACKED WITHOUT FLUSHING,
    // the worst possible shootdown bug (the sender believes the remote TLB
    // is clean). Full local flush (CR3 reload) is always correct; precision
    // is IPI_TLB_FLUSH_PAGE's job. The lazy tlb.c layer replaces this when
    // it is actually compiled (a later brick).
    write_cr3(read_cr3());

    // Acknowledge (for old-style synchronous flush API compatibility)
    __atomic_add_fetch(&tlb_flush_ack_count, 1, __ATOMIC_RELEASE);

    // Send EOI
    lapic_eoi();
}

// IPI handler: TLB flush all contexts (PCID recycle)
void ipi_handle_tlb_flush_all(void) {
    uint32_t cpu = cpu_id();
    ipi_stats[cpu].tlb_flush_received++;

    // tlb_uni.c's tlb_handle_ipi_flush_all_contexts is the same NO-OP class
    // (see the FALSE-ACK FIX above); flush ALL contexts locally for real.
    tlb_flush_all_contexts_local();

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

/* ===========================================================================
 * SMP-G2 TLBSHOOT-MIN -- bounded, ack-counted KERNEL-range shootdown.
 * ===========================================================================
 * THE PIN/NO-MIGRATION ASSUMPTION (LOUD, load-bearing -- read before G3+):
 *
 *   EVERY task in this kernel runs on exactly ONE cpu, forever. process_create
 *   and thread_create default allowed_cpus to CPU0-only (F3-2); the only CPU1
 *   residents are explicitly pinned there (pinned_cpu=1, mask=CPU1-only); fork
 *   inherits nothing (CPU0 default); there is NO migration primitive.
 *
 *   THEREFORE a USER address space is only ever loaded on its task's one CPU,
 *   and a user-mapping change needs ONLY a local invlpg on that CPU -- no
 *   cross-CPU shootdown exists for user ranges, BY CONSTRUCTION. The
 *   tlb_pinning_audit() below is the runtime gate on that construction: if a
 *   future brick widens any task's mask past one CPU, the audit FAILS the
 *   smoke and forces the general per-mm shootdown work BEFORE the assumption
 *   silently rots.
 *
 *   KERNEL/global mappings are the opposite: shared into every CR3, cached by
 *   every core's TLB, so a kernel-mapping change DOES need the cross-CPU flush
 *   below. That asymmetry -- user=local, kernel=cross -- is the entire G2
 *   model.
 *
 * LAW 16 (the lock law): the ack wait runs ONLY from lock-free context --
 * never under scheduler/rq/heap/fs locks. Enforced here by (a) an IF==1
 * entry check (necessary-not-sufficient: plain spin_lock doesn't cli, so
 * call-site review still matters), and (b) the TSC-bounded wait (a deadlock
 * degrades to a loud timeout, never a hang). tlbshoot_lock is the
 * shootdown's own dedicated serializer; holding IT across the wait is the
 * design.
 *
 * (State -- the request block, lock, and violation counter -- is defined up
 * top with the rest of the IPI .bss; constants TLBSHOOT_MAX_PAGES /
 * TLBSHOOT_WAIT_US likewise.)
 * =========================================================================== */

/* TLB_INVARIANT validator: LOG+COUNT, never panic (the F3-0 validator
 * discipline -- a false positive must not brick boot). */
void tlb_invariant_violation(const char* what) {
    g_tlb_invariant_violations++;
    kprintf("[TLB_INVARIANT] VIOLATION: %s (count=%u)\n",
            what, g_tlb_invariant_violations);
}

/* IPI handler: bounded kernel-range invlpg (the stash-mined SMP-R0 harvest,
 * now live at vector 0x57). Runs on the TARGET cpu under WHATEVER CR3 it
 * holds -- safe because the flushed range is a KERNEL range, present in every
 * address space (and invlpg of a non-cached VA is a no-op). Full CR3 reload
 * fallback for oversized/zero requests is always correct. */
void ipi_handle_tlb_flush_page(void) {
    uint32_t cpu = cpu_id();
    ipi_stats[cpu].tlb_flush_received++;

    uint64_t va = tlbshoot_addr;
    uint64_t n  = tlbshoot_npages;
    if (n == 0 || n > TLBSHOOT_MAX_PAGES) {
        write_cr3(read_cr3());                 /* full flush -- always correct */
    } else {
        for (uint64_t i = 0; i < n; i++) {
            __asm__ volatile("invlpg (%0)"
                             :: "r"((void*)(va + i * 4096ULL)) : "memory");
        }
    }

    __atomic_add_fetch(&tlbshoot_ack, 1, __ATOMIC_RELEASE);
    lapic_eoi();
}

/* Flush a KERNEL-range mapping change everywhere it can be cached: local
 * invlpg + a bounded, ack-counted IPI_TLB_FLUSH_PAGE to CPU1. Returns 0 on
 * full success, -1 if the remote side could not be confirmed (timeout /
 * law-16 refusal) -- the LOCAL flush always happens regardless.
 * Call from lock-free, IF=1 context ONLY (law 16). */
int ipi_tlb_flush_kernel_range(void* addr, uint64_t npages) {
    uint64_t va = (uint64_t)addr & ~0xFFFULL;

    /* 1. LOCAL flush first -- unconditional, bounded, always correct. */
    if (npages == 0 || npages > TLBSHOOT_MAX_PAGES) {
        write_cr3(read_cr3());
    } else {
        for (uint64_t i = 0; i < npages; i++) {
            __asm__ volatile("invlpg (%0)"
                             :: "r"((void*)(va + i * 4096ULL)) : "memory");
        }
    }

    /* 2. Remote flush only when a remote actually exists and IPIs are armed.
     * G2 model: kernel-mapping mutation is a BSP activity (CPU1 runs pinned
     * workloads); a CPU1 caller would need the mirror-image send. */
    if (!ipi_ready || !cpu1_is_online() || cpu_id() != 0) {
        return 0;
    }

    /* 3. LAW 16 entry check: an ack wait with IF=0 can deadlock (the IPI we
     * are about to wait on may be blocked behind US). Refuse the wait; the
     * local flush stands, the violation is loud, the caller learns. */
    uint64_t rf;
    __asm__ volatile("pushfq; pop %0" : "=r"(rf));
    if (!(rf & 0x200)) {
        tlb_invariant_violation("kernel-range shootdown ack wait entered with IF=0 (law 16)");
        return -1;
    }

    /* 4. Serialize the single in-flight request block, publish, kick, wait
     * BOUNDED. Spinners on tlbshoot_lock also hold no other locks (law 16
     * call-site contract), so the serializer cannot invert anything. */
    spin_lock(&tlbshoot_lock);
    tlbshoot_addr   = va;
    tlbshoot_npages = npages;
    __atomic_store_n(&tlbshoot_ack, 0, __ATOMIC_RELEASE);

    ipi_send(1, IPI_TLB_FLUSH_PAGE);

    int rc = 0;
    uint64_t s = rdtsc();
    while (__atomic_load_n(&tlbshoot_ack, __ATOMIC_ACQUIRE) == 0) {
        if ((rdtsc() - s) >= TLBSHOOT_WAIT_US * 3000ULL) {
            tlb_invariant_violation("kernel-range shootdown ack timeout (lost ack)");
            rc = -1;
            break;
        }
        __asm__ volatile("pause" ::: "memory");
    }
    if (rc == 0 &&
        __atomic_load_n(&tlbshoot_ack, __ATOMIC_ACQUIRE) > 1) {
        tlb_invariant_violation("kernel-range shootdown ack overrun (>1 acker, 2-cpu system)");
        rc = -1;
    }
    spin_unlock(&tlbshoot_lock);
    return rc;
}

/* ===========================================================================
 * SMP-G2 acceptance: tlb_shootdown_selftest() -- BSP context, CPU1 online.
 * Drives ONE real kernel-range shootdown end to end and prints the gate:
 *   TLBSHOOT: PASS kernel_flush=1 acked=1 bounded=1 invariant=1
 * then audits the pin model and prints the negative proof:
 *   TLBSHOOT_NEG: PASS no_user_crossflush_needed_under_pinning=1
 * =========================================================================== */
static uint8_t tlbshoot_testpage[4096] __attribute__((aligned(4096)));

void tlb_shootdown_selftest(void) {
    if (!ipi_ready || !cpu1_is_online()) {
        kprintf("TLBSHOOT: SKIP (cpu1 offline or IPI disarmed)\n");
        return;
    }

    /* The boot path may run cli'd here; the law-16 check would (correctly)
     * refuse the wait. The selftest's job is to prove the SHOOTDOWN, so give
     * it the legal context it demands and restore the caller's IF after. */
    uint64_t rf;
    __asm__ volatile("pushfq; pop %0" : "=r"(rf));
    if (!(rf & 0x200)) __asm__ volatile("sti");

    uint32_t v0  = g_tlb_invariant_violations;
    uint64_t rx0 = __atomic_load_n(&ipi_stats[1].tlb_flush_received, __ATOMIC_ACQUIRE);

    uint64_t t0 = rdtsc();
    int rc = ipi_tlb_flush_kernel_range(tlbshoot_testpage, 1);
    uint64_t us = (rdtsc() - t0) / 3000ULL;

    uint64_t rx1 = __atomic_load_n(&ipi_stats[1].tlb_flush_received, __ATOMIC_ACQUIRE);

    int kernel_flush = (rc == 0);
    int acked        = (rx1 == rx0 + 1);           /* CPU1's handler really ran */
    int bounded      = (us < TLBSHOOT_WAIT_US);    /* well under the 50 ms cap  */
    int invariant    = (g_tlb_invariant_violations == v0);

    if (!(rf & 0x200)) __asm__ volatile("cli");

    kprintf("TLBSHOOT: %s kernel_flush=%d acked=%d bounded=%d invariant=%d (latency_us=%lu)\n",
            (kernel_flush && acked && bounded && invariant) ? "PASS" : "FAIL",
            kernel_flush, acked, bounded, invariant, (unsigned long)us);

#ifdef SMP_RUNMASK
    /* -------- the negative proof, RUNMASK-0 UPGRADE: audit EXECUTION
     * REALITY, not declared masks. A multi-CPU allowed_cpus is now fine
     * (batchdemo); the real TLB hazard is the same ADDRESS SPACE having
     * actually RUN on more than one CPU -- which runmask_audit_crosscpu()
     * detects by aggregating ran_on_cpus per CR3 across live processes.
     * The acceptance line keeps its exact prefix so every frozen smoke's
     * grep -qF stays true. -------- */
    {
        int checked = 0;
        int cross = runmask_audit_crosscpu(&checked);
        kprintf("TLBSHOOT_NEG: %s no_user_crossflush_needed_under_pinning=%d "
                "(RUNMASK upgrade: procs_checked=%d cross_cpu_mms=%d; "
                "declared multimask OK)\n",
                (cross == 0 && checked > 0) ? "PASS" : "FAIL",
                (cross == 0 && checked > 0) ? 1 : 0, checked, cross);
    }
#else
    /* -------- the negative proof: the pin model makes user cross-flush
     * unnecessary. Walk every live process; ANY multi-CPU affinity mask
     * breaks the assumption and FAILS this gate (the loud forcing function
     * for the future per-mm shootdown work). Ctor defaults keep later
     * spawns single-CPU (F3-2: process_create/thread_create set CPU0-only;
     * fork inherits nothing). 256 == process.c's MAX_PROCESSES. -------- */
    {
        /* process_get_by_pid / process_unref come from sched.h (included). */
        int checked = 0, multi = 0;
        for (uint32_t pid = 1; pid < 256; pid++) {
            process_t* p = process_get_by_pid(pid);
            if (!p) continue;
            checked++;
            uint64_t m = p->allowed_cpus;
            /* manual popcount: freestanding -O0 turns the builtin into a
             * libgcc __popcountdi2 call, which is not linked */
            int bits = 0;
            for (uint64_t mm = m; mm; mm &= (mm - 1)) bits++;
            if (bits > 1) {
                multi++;
                kprintf("[TLB_INVARIANT] pin audit: pid=%d '%s' has MULTI-CPU "
                        "mask 0x%llx -- user cross-flush now REQUIRED\n",
                        p->pid, p->name, (unsigned long long)m);
            }
            process_unref(p);
        }
        kprintf("TLBSHOOT_NEG: %s no_user_crossflush_needed_under_pinning=%d "
                "(procs_checked=%d multi_cpu_masks=%d)\n",
                (multi == 0 && checked > 0) ? "PASS" : "FAIL",
                (multi == 0 && checked > 0) ? 1 : 0, checked, multi);
    }
#endif
}

#ifdef SMP_RUNMASK
/* ===========================================================================
 * SMP-RUNMASK-0 -- the audit audits REALITY.
 * ===========================================================================
 * The G2 mask heuristic (multi-CPU allowed_cpus = danger) went stale the day
 * F3-7 shipped the first legitimate multimask task. The TRUE invariant:
 *   an ADDRESS SPACE that actually executed on >1 CPU = the TLB hazard.
 * ran_on_cpus is stamped at the single dispatch chokepoint
 * (cpu_set_current_thread); this audit aggregates it per CR3 (threads share
 * an mm -- the address space, not the PCB, is the unit) across live
 * processes. Kernel-CR3 residents (idle threads, kthreads) are EXCLUDED:
 * the kernel address space legitimately runs on every CPU and is exactly
 * what the G2 kernel-range shootdown protects.
 * =========================================================================== */
volatile uint32_t g_runmask_exit_multimask = 0;  /* dying multimask processes  */
volatile uint32_t g_runmask_exit_crosscpu  = 0;  /* ...that ran on >1 CPU (BAD) */

#define RUNMASK_MAX_MMS 128
static uint64_t runmask_a_cr3[RUNMASK_MAX_MMS];   /* low .bss scratch */
static uint32_t runmask_a_ran[RUNMASK_MAX_MMS];

/* Walk live processes, aggregate ran_on_cpus per user CR3, count address
 * spaces that executed on >1 CPU (each reported LOUDLY). checked_out gets
 * the number of live user processes walked. Returns the violation count. */
int runmask_audit_crosscpu(int* checked_out) {
    uint64_t kernel_cr3 = read_cr3();   /* BSP kernel context == the shared kernel space */
    int n = 0, checked = 0, viol = 0;

    for (uint32_t pid = 1; pid < 256; pid++) {
        process_t* p = process_get_by_pid(pid);
        if (!p) continue;
        uint64_t cr3 = p->context.cr3;
        if (cr3 == 0 || cr3 == kernel_cr3) {   /* kernel-space resident: exempt */
            process_unref(p);
            continue;
        }
        checked++;
        int i;
        for (i = 0; i < n; i++) {
            if (runmask_a_cr3[i] == cr3) break;
        }
        if (i == n && n < RUNMASK_MAX_MMS) {
            runmask_a_cr3[n] = cr3;
            runmask_a_ran[n] = 0;
            n++;
        }
        if (i < n) runmask_a_ran[i] |= p->ran_on_cpus;
        process_unref(p);
    }

    for (int i = 0; i < n; i++) {
        uint32_t r = runmask_a_ran[i];
        int bits = 0;
        for (uint32_t rr = r; rr; rr &= (rr - 1)) bits++;
        if (bits > 1) {
            viol++;
            kprintf("[RUNMASK] VIOLATION: address space cr3=0x%llx EXECUTED on "
                    "%d CPUs (ran=0x%x) -- user cross-flush now REQUIRED\n",
                    (unsigned long long)runmask_a_cr3[i], bits, r);
        }
    }

    if (checked_out) *checked_out = checked;
    return viol;
}

/* The forced-detection proof: PLANT a cross-CPU footprint on a live PCB
 * (init), assert the audit catches it, restore, assert clean again. Never
 * actually runs one mm on two CPUs -- the plant IS the synthetic case. */
void runmask_selftest(void) {
    int c0 = 0, c1 = 0, c2 = 0;
    int v0 = runmask_audit_crosscpu(&c0);            /* baseline: clean      */

    int planted = 0, detected = 0;
    process_t* ini = process_get_by_pid(1);
    if (ini) {
        uint32_t save = ini->ran_on_cpus;
        ini->ran_on_cpus = 0x3;                      /* the forced case      */
        planted = 1;
        detected = (runmask_audit_crosscpu(&c1) == v0 + 1);
        ini->ran_on_cpus = save;                     /* restore reality      */
        process_unref(ini);
    }
    int restored_clean = (runmask_audit_crosscpu(&c2) == v0);

    int pass = (v0 == 0) && planted && detected && restored_clean && (c0 > 0);
    kprintf("RUNMASK-CORE: %s baseline_clean=%d forced_crosscpu_detected=%d "
            "restored_clean=%d (user_procs=%d)\n",
            pass ? "PASS" : "FAIL",
            (v0 == 0) ? 1 : 0, detected, restored_clean, c0);
}
#endif /* SMP_RUNMASK */

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
