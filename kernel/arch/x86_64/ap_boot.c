/*
 * ap_boot.c -- SMP brick 3/5/6: bring ONE application processor (CPU 1) online,
 * then run a managed kernel worker loop that executes a SINGLE trusted job the BSP
 * dispatches into one job slot. GATED behind -DSMP_FOUNDATION (SMP=1).
 * ============================================================================
 *
 * Scope (deliberately tiny):
 *   - Copy the 16-bit real-mode AP trampoline (ap_trampoline.asm) to a low,
 *     page-aligned, real-mode-reachable address (0x8000 -> SIPI vector 0x08).
 *   - Hand the AP its stack, the BSP's CR3 (the AP runs in the SAME page
 *     tables), the kernel's runtime GDTR/IDTR images, and the 64-bit entry.
 *   - Send INIT-SIPI-SIPI to CPU 1's APIC id (from the ACPI MADT) via the LAPIC
 *     ICR (MMIO, already enabled + proven in brick 1).
 *   - Wait with a BOUNDED TSC-deadline timeout (~100 ms) polling a shared
 *     MEMORY flag the AP sets. NEVER an infinite spin, NEVER a panic.
 *   - The AP marks itself online, bumps its isolated per-CPU heartbeat counter
 *     (cpu_hb[1]) a SMALL fixed number of times as proof-of-life, then (brick 6)
 *     enters a MANAGED WORKER loop: it spin-polls ONE job slot and, when the BSP
 *     publishes a job, runs that ONE trusted fn and signals done -- otherwise it
 *     bumps idle_ticks and `pause`s. It cannot `hlt`-park because it has no wakeup
 *     IRQ yet (IPI-wake is a later brick), so it busy-polls to stay responsive.
 *
 * THE HARD RULE: an AP that never comes up MUST NOT stop or hang the BSP. Every
 * wait here is a finite deadline; on timeout the caller logs the failure and the
 * BSP keeps booting single-core. There is no path in this file that can block
 * the BSP indefinitely or panic.
 *
 * NO scheduling, NO AP timer, NO per-CPU runqueue, NO task migration -- those
 * are later bricks. The AP touches NOTHING stateful (no scheduler, no allocator
 * call from the AP itself; its stack is a static buffer reserved at link time).
 *
 * Self-contained on purpose (mirrors madt.c): it includes only what it needs and
 * declares kernel helpers via extern. In particular it does NOT define cpu_id()
 * -- that symbol lives in stubs.c for the default build, and the AP identifies
 * itself with lapic_get_id() (brick 1) instead, so there is no duplicate-symbol
 * collision with smp.c (which is never compiled).
 */

#include "../../include/types.h"
#include "../../include/kernel.h"   /* kprintf */
#include "../../include/string.h"   /* memcpy  */
#include "../../include/x86_64.h"   /* read_cr3 */
#include "../../include/perf.h"     /* rdtsc   */
#include "../../include/kref.h"     /* kget/kput for buffer lifecycle */
#include "../../include/ownership.h" /* ownership_t state tracking */
#include "../../include/spinlock.h" /* spinlock_t for cpu1_job_lock */
#include "../../include/errno.h"    /* EAPFAULT */
#include "../../include/sched.h"    /* process_t for current_process */
#include "../../include/smp.h"      /* percpu_data_t for health monitor */
#ifdef SMP_SCHED
#include "../../include/lapic_constants.h"  /* AP_LAPIC_TIMER_VECTOR (Brick E) */
#endif

/* Forward declaration for process ownership metadata in cpu1_job. */
typedef struct process process_t;

/* Current process (from sched.h), used to track job ownership. */
extern process_t* current_process;

/* cpu_id() helper (from smp.h) for ownership state transitions. */
extern uint32_t cpu_id(void);

/* percpu_data array (from smp.h) for health monitoring (heartbeat checks).
 * The typedef is in smp.h, so we just declare the array here. */

/* ---------------------------------------------------------------------------
 * Trampoline relocation contract. These MUST match ap_trampoline.asm
 * (AP_TRAMPOLINE_BASE / AP_PARAM_OFFSET) and the param-block field offsets.
 *
 * 0x8000 is below 1 MB, page-aligned, and outside the IVT/BDA/EBDA we read, so
 * it is a safe SIPI target page. SIPI vector = 0x8000 >> 12 = 0x08.
 * ------------------------------------------------------------------------- */
#define AP_TRAMPOLINE_BASE   0x8000UL
#define AP_PARAM_OFFSET      0x0F00UL
#define AP_SIPI_VECTOR       ((uint32_t)(AP_TRAMPOLINE_BASE >> 12))  /* 0x08 */

/* Param-block field offsets (relative to AP_TRAMPOLINE_BASE + AP_PARAM_OFFSET).
 * Layout (see ap_trampoline.asm ap_param_block):
 *   +0  cr3   +8  stack_top   +16 entry   +24 arg   +32 gdtr(16)   +48 idtr(16) */
#define AP_PARAM_CR3         0
#define AP_PARAM_STACK_TOP   8
#define AP_PARAM_ENTRY       16
#define AP_PARAM_ARG         24
#define AP_PARAM_GDTR        32
#define AP_PARAM_IDTR        48

/* The AP's stack. A static, 16-byte-aligned .bss buffer (16 KiB). The AP runs in
 * the BSP's page tables, so this kernel .bss buffer is already mapped and
 * reachable from the AP -- no allocation needed for a bare heartbeat. */
#define AP_STACK_SIZE        16384
static uint8_t ap_stack[AP_STACK_SIZE] __attribute__((aligned(16)));

/* ---------------------------------------------------------------------------
 * The AP online handshake. `ap1_online` lives in plain kernel memory (.bss). The
 * AP stores 1 with SEQ_CST; the BSP polls it with ACQUIRE in a bounded loop.
 * This is a MEMORY flag -- the BSP does NOT poll any MMIO/APIC register while
 * waiting, so a wedged LAPIC cannot make the BSP spin forever.
 * ------------------------------------------------------------------------- */
static volatile uint32_t ap1_online = 0;

/* ---------------------------------------------------------------------------
 * AP panic flag. Set by the AP if it panics during job execution.
 * The BSP checks this in cpu1_wait() to detect AP crashes.
 * ------------------------------------------------------------------------- */
static volatile uint32_t cpu1_panic_flag = 0;

/* AP crash context capture. Stores the last exception that hit CPU1. */
typedef struct {
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t rsp;
    uint64_t cr2;
    uint64_t cr3;
} ap_crash_context_t;

static volatile ap_crash_context_t cpu1_crash_ctx = {0};

/* ---------------------------------------------------------------------------
 * CPU1 offline flag. Marks CPU1 as offline after a panic.
 * This is the SMP_FOUNDATION-build equivalent of the full smp.c state tracking.
 * ------------------------------------------------------------------------- */
static volatile uint32_t cpu1_offline = 0;

/* Simplified smp_cpu_offline for SMP_FOUNDATION build (self-contained in this file).
 * Marks CPU1 as offline and prevents future job submissions to it. */
static void smp_cpu_offline(uint32_t cpu)
{
    if (cpu == 1) {
        __atomic_store_n(&cpu1_offline, 1, __ATOMIC_RELEASE);
    }
}

/*
 * cpu1_panic_handler -- AP-side panic handler called when CPU1 crashes.
 *
 * Captures the exception context, sets the panic flag for the BSP to detect,
 * and halts the AP. The BSP polls cpu1_panic_flag in cpu1_wait() and will
 * observe the crash, log the context, mark CPU1 offline, and return EAPFAULT.
 *
 * This is the AP's controlled crash path: it does NOT call kernel_panic()
 * (which would try to reset/halt the whole system), but instead signals the
 * BSP that it crashed and then halts itself. The BSP and the rest of the
 * system continue running single-core.
 *
 * MUST be called from CPU1 only (the AP), with interrupts disabled.
 */
void cpu1_panic_handler(uint64_t vector, uint64_t error_code,
                        uint64_t rip, uint64_t rsp, uint64_t cr2, uint64_t cr3)
{
    /* Capture crash context for the BSP to display. */
    cpu1_crash_ctx.vector     = vector;
    cpu1_crash_ctx.error_code = error_code;
    cpu1_crash_ctx.rip        = rip;
    cpu1_crash_ctx.rsp        = rsp;
    cpu1_crash_ctx.cr2        = cr2;
    cpu1_crash_ctx.cr3        = cr3;

    /* Set panic flag to signal the BSP. SEQ_CST ensures visibility. */
    __atomic_store_n(&cpu1_panic_flag, 1, __ATOMIC_SEQ_CST);

    /* Halt this AP. The BSP will detect the panic flag and continue single-core. */
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

/* ---------------------------------------------------------------------------
 * SMP brick 3.5: independent PER-CPU heartbeat counters (isolation proof).
 *
 * Two counters, ONE per logical CPU, placed on SEPARATE 64-byte cache lines so
 * that CPU0 (BSP) and CPU1 (AP) each only ever touch their OWN line -- this is
 * the whole point: it proves two cores can safely hammer ISOLATED per-CPU memory
 * with ZERO shared scheduling/allocator state and ZERO false sharing. The 56-byte
 * pad fills out the line after the 8-byte counter; aligned(64) guarantees each
 * struct starts on its own line, so cpu_hb[0] and cpu_hb[1] never collide.
 *
 *   cpu_hb[0] = BSP heartbeat (incremented by the BSP proof window in kernel.c)
 *   cpu_hb[1] = AP  heartbeat (incremented forever by ap_main() below)
 *
 * Exported (non-static) so kernel.c can read/increment cpu_hb[0] and display both.
 * NOT volatile at the struct level -- only the counter field is volatile, which is
 * enough to keep the compiler from hoisting the increments/reads out of the loops.
 */
struct hb { volatile uint64_t v; char pad[56]; } __attribute__((aligned(64)));
struct hb cpu_hb[2] = {{0, {0}}, {0, {0}}};   /* [0]=BSP, [1]=AP; 64-byte-separated */

/* ---------------------------------------------------------------------------
 * SMP brick 5/6: the AP's idle tick counter.
 *
 * This is the counter CPU 1 OWNS in its kernel worker loop (ap_main below). In
 * brick 5 the AP hlt-parked (this settled at ~1). In brick 6 the AP spin-polls the
 * job slot, so it bumps this counter once per idle poll while it WAITS for work --
 * the counter now climbs. This is the honest cost of brick 6: the AP has NO wakeup
 * interrupt yet (a low-power IPI-wake that would let it `hlt` between jobs is a
 * later brick), so to stay responsive to the slot it must busy-poll, not park.
 *
 * Design note (standalone vs cpu_t.idle_ticks): the per-CPU cpu_t/cpus[] array is
 * file-local to scheduler.c and is indexed by cpu_id(), which still returns 0 on the
 * AP (stubs.c), so the AP cannot index its own slot without a new lapic-id-based
 * accessor exported across files. This file is deliberately self-contained (see the
 * header), so we keep the brick-5 counter standalone here -- mirroring cpu_hb -- to
 * avoid cross-file coupling for a single uint64_t. It is exported (non-static) so the
 * BSP can read/display it later. Only the field is volatile, which is enough to keep
 * the compiler from optimizing the single bump away.
 */
volatile uint64_t ap1_idle_ticks = 0;   /* CPU 1's managed-idle wakeup counter */

/* SMP observability counters (Phase-1 proof scaffolding for the CPU1 coprocessor
 * mailbox; the foundation the per-CPU scheduler work is validated against). These are
 * PURE COUNTERS -- they change NO dispatch behavior, only record what happened:
 *   cpu1_dispatch_total = jobs handed to CPU1 (a successful cpu1_submit),
 *   cpu1_complete_total = jobs CPU1 finished within the deadline (cpu1_wait success),
 *   cpu1_timeout_total  = jobs that did NOT signal done in the bounded deadline (a lost
 *                         wakeup / stuck job from the BSP's view).
 * Non-static so a future read-only stats path can extern them. For the polled mailbox
 * "stuck_tasks" is always 0: every wait is bounded, so nothing stays pending forever. */
volatile uint64_t cpu1_dispatch_total = 0;
volatile uint64_t cpu1_complete_total = 0;
volatile uint64_t cpu1_timeout_total  = 0;

/* ---------------------------------------------------------------------------
 * SMP brick 6: the ONE job slot the BSP uses to dispatch a SINGLE trusted kernel
 * worker function to CPU 1. This is the first REAL work on the second core.
 *
 * Scope (deliberately one slot, deliberately tiny):
 *   - The BSP fills fn/arg, clears `done`, then publishes `pending`.
 *   - The AP worker loop (ap_main) ACQUIRE-loads `pending`; on 1 it runs fn(arg),
 *     publishes `done`, then clears `pending`.
 *   - The BSP spins on `done` with a BOUNDED ~100 ms TSC deadline (cpu1_run).
 *
 * CPU 1 runs ONLY the trusted `fn` the BSP set here -- there is NO scheduler, NO
 * task queue, NO arbitrary process, NO migration. One slot, one trusted callback.
 *
 * MEMORY ORDERING (why the AP can never run a stale fn/arg, nor miss a job):
 *   - BSP (cpu1_run): writes fn, arg, then `done=0` (RELAXED), then `pending=1`
 *     with RELEASE. The RELEASE store on `pending` is a one-way barrier: every
 *     prior write (fn, arg, done) is visible to any thread that ACQUIRE-loads the
 *     SAME `pending` and sees the 1.
 *   - AP (ap_main): ACQUIRE-loads `pending`; observing 1 synchronizes-with the
 *     BSP's RELEASE, so the fn/arg it then reads are guaranteed the freshly
 *     published values, never a torn or stale prior pair. It cannot "miss" a job:
 *     it spin-polls the slot every iteration (pause), so any published `pending=1`
 *     is seen on a subsequent load. After running, it RELEASE-stores `done=1`
 *     (publishing g_worktest / any fn side effects) and then clears `pending`.
 *   - BSP wait: ACQUIRE-loads `done`; observing 1 synchronizes-with the AP's
 *     RELEASE, so all of fn's shared-memory writes are visible before cpu1_run
 *     returns 1. `done` is cleared BEFORE `pending` is set, so the BSP can never
 *     observe a leftover `done=1` from a previous job.
 * Both fields are `volatile` AND accessed via __atomic_* (acquire/release) so the
 * compiler neither reorders nor elides them and the CPU's ordering is enforced.
 * ------------------------------------------------------------------------- */
typedef void (*cpu_job_fn)(void *);
static struct {
    cpu_job_fn       fn;
    void            *arg;
    volatile int     pending;
    volatile int     done;
    int              owner_cpu;
    process_t       *owner_proc;
    ownership_t      arg_A;         // ownership-tracked arg (TRANSFERRED CPU0->CPU1)
    ownership_t      arg_B;         // ownership-tracked arg (TRANSFERRED CPU0->CPU1)
    ownership_t      result;        // ownership-tracked result (CPU1->CPU0)
    uint64_t         job_seq;       // guards slot reuse before orphaned job drains
    uint32_t         owner_pid;     // submitting process PID (for orphan cleanup)
} cpu1_job;

/* Spinlock protecting cpu1_job slot from concurrent access by multiple CPU0
 * submitters (e.g. concurrent syscall offloads from different processes). The
 * critical section is cpu1_submit's write to fn/arg/owner_pid/job_seq plus the
 * ownership transitions and the final RELEASE-store of pending. Initialized to
 * unlocked (zero, matching the static initializer). */
static spinlock_t cpu1_job_lock = {0};

/* Tiny proof-of-life: how many times ap_main bumps cpu_hb[1] BEFORE parking in the
 * managed idle. Just enough that the existing brick-3.5 "CPU1 ran" evidence (a
 * nonzero cpu_hb[1]) still shows the AP executed, without it being a busy spinner. */
#define AP_PROOF_OF_LIFE_TICKS 8u

/* 10-byte descriptor-table register image (limit + base), matching sgdt/sidt. */
typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} dtr_image_t;

/* Symbols emitted by ap_trampoline.asm (the trampoline code blob). */
extern uint8_t ap_trampoline_start[];
extern uint8_t ap_trampoline_end[];

/* Brick-1 LAPIC helpers (kernel/arch/x86_64/lapic.c). */
extern uint32_t lapic_get_id(void);
extern void     lapic_send_init(uint32_t apic_id);
extern void     lapic_send_startup(uint32_t apic_id, uint32_t vector);

/* Brick-0 MADT helpers (kernel/arch/x86_64/madt.c). */
extern int madt_get_apic_id(int index);   /* APIC id of Nth enabled CPU, or -1 */

#ifdef SMP_SCHED
/* ===========================================================================
 * SMP SCHEDULER Brick A: a REAL cpu_id().
 * ---------------------------------------------------------------------------
 * The coprocessor build uses stubs.c::cpu_id()==0 (single logical CPU). With
 * -DSMP_SCHED we instead map the running core's hardware xAPIC id to a small
 * logical id (BSP->0, AP1->1) so per-CPU state (current process, runqueue, TSS)
 * can be indexed by CPU. We capture CPU1's hardware APIC id ONCE in
 * try_start_cpu1() (BSP context, from the MADT) into g_ap1_apic_id; cpu_id()
 * then reads its own xAPIC id via lapic_get_id() and compares.
 *
 * Ordering note: before try_start_cpu1() runs, g_ap1_apic_id is the sentinel
 * 0xFFFFFFFF, so cpu_id() returns 0 for everyone -- identical to the old stub.
 * After capture, only the core whose xAPIC id == g_ap1_apic_id reports 1; the
 * BSP (and any unexpected core) reports 0. lapic_get_id() is a cheap MMIO read
 * of LAPIC[0x20]>>24, valid on both cores once their LAPIC is enabled (the BSP
 * in lapic_init(), the AP in the trampoline before ap_main). */
static volatile uint32_t g_ap1_apic_id = 0xFFFFFFFFu;  /* CPU1 hw APIC id (set in try_start_cpu1) */

uint32_t cpu_id(void)
{
    uint32_t self = lapic_get_id();
    if (self == __atomic_load_n(&g_ap1_apic_id, __ATOMIC_ACQUIRE)) {
        return 1u;
    }
    return 0u;   /* BSP, or any core we have not enumerated -> logical 0 */
}

/* Checkpoint evidence: the AP stores its own cpu_id() here so the BSP can print
 * it MP-safely (ap_main must NOT touch the serial port -- two cores racing on
 * 0x3F8 is not MP-safe). Expected value after CPU1 comes online: 1. */
volatile uint32_t g_ap_observed_cpuid = 0xFFFFFFFFu;

/* Brick B: the AP loads its OWN TSS (selector 0x38) via this gdt.c helper. */
extern void gdt_ap_load_tss(void);

/* Brick B checkpoint: the AP records its loaded Task Register selector here (via
 * `str`) so the BSP can print it MP-safely. Expected after gdt_ap_load_tss(): 0x38. */
volatile uint16_t g_ap_observed_tr = 0xFFFFu;

/* Read the current Task Register selector (str = Store Task Register). */
static inline uint16_t ap_read_tr(void)
{
    uint16_t tr;
    __asm__ volatile("str %0" : "=r"(tr));
    return tr;
}

/* Brick E: CPU1 LAPIC timer tick count. Only CPU1 writes it (single-writer), the
 * BSP reads it for the checkpoint -> volatile is enough, no lock. */
volatile uint64_t ap_timer_ticks = 0;

/*
 * lapic_tick -- the C handler for CPU1's LAPIC timer ISR (ap_lapic_timer_isr).
 * `frame` is the interrupt_frame_t the asm stub built (used by Brick F to switch).
 *
 * BRICK E (this brick): prove the timer + EOI plumbing ONLY. Bump the per-CPU tick
 * counter and EOI the LAPIC (NOT the PIC -- a PIC EOI here would wedge the BSP's
 * IRQ0). Do NOT call schedule_from_irq yet, so the frame is left untouched and the
 * iretq simply resumes whatever CPU1 was doing (its coprocessor worker loop).
 */
void lapic_tick(void* frame)
{
    ap_timer_ticks++;
    extern void lapic_eoi(void);
    lapic_eoi();                 /* LAPIC EOI (lapic_write(LAPIC_EOI, 0)) FIRST */

#if defined(SMP_SCHED_DISPATCH)
    /* Brick F: drive CPU1's preemptive dispatch. The EOI above already happened, so
     * the LAPIC is ready for the next tick regardless of which process we resume.
     * ap_schedule_from_irq is the AP-safe dispatcher (no global current, no PIC EOI);
     * it may rewrite the frame in place to switch processes (F3+). In F1 its ring-3
     * guard returns immediately (CPU1 is in its ring-0 scheduler loop). */
    /* ap_schedule_from_irq is declared in sched.h (included above) under this gate. */
    ap_schedule_from_irq((interrupt_frame_t*)frame);
#else
    (void)frame;
#endif
}
#endif /* SMP_SCHED */

/* TSC convention used across the SMP bricks: ~3 GHz estimate => 3000 cycles/us.
 * Used ONLY to bound the wait; the exact real frequency does not matter because
 * we only need a finite deadline (faster TSC -> shorter wait, slower -> longer,
 * but always bounded). */
#define AP_TSC_PER_US        3000ULL
#define AP_WAIT_US           100000ULL    /* 100 ms bounded timeout */

/* Short fixed spin for the INIT->SIPI (>=10 ms) and SIPI->SIPI (>=200 us)
 * inter-IPI settle delays. We use a TSC deadline here too so the delay is a real
 * wall-clock interval rather than an unpredictable instruction count. */
static void ap_tsc_delay_us(uint64_t us)
{
    uint64_t start = rdtsc();
    uint64_t want  = us * AP_TSC_PER_US;
    while ((rdtsc() - start) < want) {
        __asm__ volatile("pause" ::: "memory");
    }
}

/*
 * ap_main -- the 64-bit C entry the trampoline jumps to once the AP is in long
 * mode with the kernel GDT/IDT/CR3 and its own stack (RDI = logical cpu id, here
 * always 1). Keep this MINIMAL: mark online, prove it ran, then PARK in a managed
 * kernel idle loop (low power), NOT a busy spin.
 *
 * Intentionally does NOT log (two CPUs racing on the 0x3F8 serial port is not
 * MP-safe) -- the BSP prints "CPU 1 online" / "CPU 1 -> managed idle" after it
 * observes the flag. Does NOT touch the scheduler, allocator, or any stateful
 * subsystem. Does NOT enable interrupts (NO sti, NO timer, NO handler).
 *
 * BRICK 6 (this brick): after publishing online (SEQ_CST so the BSP's ACQUIRE load
 * sees it), the AP first bumps cpu_hb[1] a SMALL fixed number of times -- a
 * proof-of-life so the existing brick-3.5 "CPU1 ran" evidence (nonzero cpu_hb[1])
 * still holds and shows the AP executed -- and then enters its MANAGED WORKER LOOP:
 *
 *     for (;;) {
 *         if (ACQUIRE-load cpu1_job.pending) {        // BSP published a job
 *             cpu1_job.fn(cpu1_job.arg);              // run the ONE trusted fn
 *             RELEASE-store cpu1_job.done = 1;        // publish results
 *             RELEASE-store cpu1_job.pending = 0;     // free the slot
 *         } else {
 *             ap1_idle_ticks++;                       // idle this round
 *             __asm__ volatile("pause");              // spin-poll hint
 *         }
 *     }
 *
 * The AP SPIN-POLLS the single job slot (it CANNOT take an IPI -- there are NO
 * interrupts armed on the AP yet, so a low-power IPI-wake is a LATER brick). It
 * stays out of `hlt` precisely so it remains responsive to the slot. It runs ONLY
 * the trusted `fn` the BSP set in cpu1_job -- nothing else: NO scheduler, NO task
 * queue, NO arbitrary process, NO migration. See the cpu1_job comment above for the
 * full acquire/release reasoning (the AP can never run a stale fn/arg nor miss a
 * job). ap1_idle_ticks now climbs while the AP waits for work (it is no longer
 * hlt-parked at ~1) -- that is the honest cost of having no wakeup IRQ yet.
 */
void ap_main(uint64_t cpu)
{
    (void)cpu;

#ifdef SMP_SCHED
    /* Brick B: load CPU1's OWN TSS (selector 0x38) FIRST, before anything else and
     * before any sti. From here CPU1's ring-3 entries / #DF use ITS rsp0 + ITS IST1
     * stack, never CPU0's. ltr is CPU-local and needs no interrupts; the BSP already
     * installed gdt[7-8] in tss_init(). */
    gdt_ap_load_tss();
    g_ap_observed_tr = ap_read_tr();         /* expect 0x38 */

    /* Brick C: program CPU1's OWN SYSCALL MSRs (STAR/LSTAR/FMASK/EFER.SCE are
     * per-CPU). CPU1's LSTAR -> syscall_entry_cpu1, which loads kernel_rsp_save_arr[1]
     * -- so when CPU1 runs ring-3 code (Brick F) its SYSCALLs use ITS kernel stack,
     * never CPU0's. Harmless now (CPU1 issues no SYSCALL until it dispatches). */
    {
        extern void syscall_msr_init_ap(void);
        syscall_msr_init_ap();
    }

#ifdef SMP_SCHED_DISPATCH
    /* Brick F: ENABLE SSE on CPU1. The AP trampoline brings up long mode but does
     * NOT enable SSE (CR4.OSFXSR), and context_switch_asm uses fxsave64/fxrstor64 to
     * save/restore FPU state on EVERY context switch -- those #UD/#GP if SSE is off,
     * silently wedging CPU1 the instant ap_enter_scheduler does its first switch.
     * Set CR0.MP=1, CR0.EM=0 (no FPU emulation) and CR4.OSFXSR|OSXMMEXCPT (enable
     * fxsave/fxrstor + SSE), matching what the BSP did at boot. Per-CPU control regs. */
    {
        uint64_t cr0, cr4;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 &= ~(1ULL << 2);                 /* EM = 0 (no x87 emulation) */
        cr0 |=  (1ULL << 1);                 /* MP = 1 */
        __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1ULL << 9) | (1ULL << 10);   /* OSFXSR | OSXMMEXCPT */
        __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
    }
#endif

    /* Brick A checkpoint: record what THIS core's REAL cpu_id() reports BEFORE we
     * publish ap1_online. Because the SEQ_CST store of ap1_online below acts as a
     * release, the BSP's ACQUIRE-load of ap1_online synchronizes-with this store,
     * so by the time the BSP prints the checkpoint it is guaranteed to observe the
     * value here (expect 1) rather than the .bss sentinel. g_ap1_apic_id was
     * published by the BSP (RELEASE) before the SIPI that started us, so cpu_id()'s
     * ACQUIRE-load of it sees CPU1's hardware APIC id. */
    g_ap_observed_cpuid = cpu_id();
#endif

    /* Tell the BSP we reached long mode and are alive. SEQ_CST publish. */
    __atomic_store_n(&ap1_online, 1u, __ATOMIC_SEQ_CST);

    /* Proof-of-life: a SMALL, bounded number of heartbeat bumps so the brick-3.5
     * "the AP executed" evidence (cpu_hb[1] != 0) still shows -- but NOT a busy
     * spin. `pause` is just the spin-wait hint for these few iterations. */
    for (unsigned i = 0; i < AP_PROOF_OF_LIFE_TICKS; i++) {
        cpu_hb[1].v++;
        __asm__ volatile("pause");
    }

#ifdef SMP_SCHED
    /* Brick E: arm CPU1's OWN LAPIC timer (100 Hz) at the dedicated vector 0x40 and
     * ENABLE INTERRUPTS for the first time on CPU1. From here the timer ISR fires
     * periodically -> lapic_tick() EOIs the LAPIC + bumps ap_timer_ticks, then iretq
     * resumes the worker loop below. No scheduling yet (Brick F). The PIC delivers
     * legacy IRQs to the BSP only, and CPU1's other LVTs are masked at reset, so the
     * ONLY interrupt CPU1 takes is its local timer (and, handled, a 0xFF spurious).
     * This is the FIRST sti on CPU1. */
    {
        /* Software-enable CPU1's OWN LAPIC first (set SIVR.enable + TPR=0 + the
         * IA32_APIC_BASE global-enable). The AP never ran lapic_init(), so its LAPIC
         * is software-disabled and would deliver no timer interrupts. xAPIC MMIO is
         * CPU-local (no x2APIC here), so this programs the AP's own LAPIC. */
        extern void lapic_enable(void);
        extern void lapic_timer_init_vector(uint32_t frequency, uint8_t vector);
        lapic_enable();
        lapic_timer_init_vector(100, AP_LAPIC_TIMER_VECTOR);
        __asm__ volatile("sti");
    }
#endif

#if defined(SMP_SCHED) && defined(SMP_SCHED_DISPATCH)
    /* SCHEDULER MODE (Brick F): CPU1 hands itself to the AP-safe scheduler loop
     * instead of the coprocessor worker loop. From here CPU1's LAPIC timer drives
     * ap_schedule_from_irq() and the loop cooperatively runs CPU1's own runqueue
     * processes. CPU1 no longer services cpu1_job (the BSP skips the matmul/worktest
     * offload self-tests in this mode). ap_scheduler_loop() NEVER returns. */
    {
        /* SCHEDULER MODE: run ap_scheduler_loop directly on the AP boot stack. CPU1's
         * idle thread (cpus[1].current_thread, set in scheduler_init_secondary_cpu)
         * is the SAVE TARGET for this loop's execution: the first cooperative switch
         * (idle->thread) saves THIS boot-stack state into idle->context, and a later
         * switch back restores it -- self-consistent (idle always resumes the loop on
         * the boot stack, which is a valid static buffer). NEVER returns. */
        extern void ap_scheduler_loop(void);
        ap_scheduler_loop();
        kernel_panic("ap_scheduler_loop returned (must never happen)");
    }
#endif

    /* COPROCESSOR MODE (default SMP_SCHED / SMP=1): managed kernel WORKER loop.
     * Poll the ONE job slot the BSP dispatches into. On a pending job, run the
     * trusted fn, publish `done`, then free the slot. Otherwise idle (bump the tick
     * counter, `pause`). It executes ONLY cpu1_job.fn -- NO scheduler. */
    for (;;) {
        if (__atomic_load_n(&cpu1_job.pending, __ATOMIC_ACQUIRE)) {
            /* ACQUIRE on `pending`==1 synchronizes-with the BSP's RELEASE in
             * cpu1_run, so fn/arg read here are the freshly published values. */
            cpu1_job.fn(cpu1_job.arg);
            /* Publish the job's results (e.g. g_worktest) BEFORE marking done so
             * the BSP's ACQUIRE-load of `done` sees all of fn's writes. */
            __atomic_store_n(&cpu1_job.done, 1, __ATOMIC_RELEASE);
            /* Free the slot last. */
            __atomic_store_n(&cpu1_job.pending, 0, __ATOMIC_RELEASE);
        } else {
            ap1_idle_ticks++;
            __asm__ volatile("pause");
        }
    }
}

/* ---------------------------------------------------------------------------
 * SMP brick 6 self-test job. A trivial, TRUSTED kernel function dispatched to
 * CPU 1 exactly once after it comes online, to PROVE the AP executes BSP-supplied
 * code and returns a correct result through shared memory. It sums 1..n on CPU 1
 * and writes the total to g_worktest; the BSP checks g_worktest == 500500 for
 * n=1000. Both symbols are non-static so kernel.c's self-test can reference them
 * (this file is otherwise self-contained; these two are its only cross-file exports
 * for the test, mirroring how cpu_hb is exported). `g_worktest` is volatile so the
 * compiler does not elide the store the BSP then reads from another core; the
 * cross-core visibility ordering is provided by the done RELEASE/ACQUIRE pair. */
volatile long g_worktest = 0;
void worktest(void *a)
{
    long n = (long)a;
    long s = 0;
    for (long i = 1; i <= n; i++) {
        s += i;
    }
    g_worktest = s;
}

/*
 * SMP brick 8: split the blocking primitive into SUBMIT + WAIT so the BSP and the
 * AP can run their halves of a job CONCURRENTLY (the BSP submits, then does its own
 * share of the work, then waits). cpu1_run (brick 6) is preserved below as exactly
 * "submit then wait on the default deadline", so its existing call site is unchanged.
 *
 * cpu1_submit -- publish the job and return IMMEDIATELY (NON-blocking).
 *
 * Ordering: write fn/arg, then clear `done` (RELAXED -- it is republished under the
 * `pending` RELEASE fence below), then RELEASE-store `pending=1`. The RELEASE store
 * is a one-way barrier: when the AP ACQUIRE-observes pending==1 it is guaranteed to
 * also see the fn/arg/done we wrote first, so it can never run a stale fn or observe
 * a leftover done=1 from a previous job. Returns 1 if a job was published, 0 if fn
 * was NULL (nothing to wait on).
 *
 * OWNERSHIP INTEGRATION (SMP-foundation discipline):
 *   - arg_A/arg_B: CPU0 (BSP) initializes OWNED, transitions to TRANSFERRED(to=1)
 *     at publish. CPU1 (AP) consumes them as TRANSFERRED-in, transitions to OWNED
 *     after reading (or ORPHANED on job completion if not needed further).
 *   - result: CPU1 initializes OWNED, transitions to TRANSFERRED(to=0) when done.
 *     CPU0 transitions back to OWNED when wait completes successfully.
 *   - On timeout (cpu1_wait fails): arg_A/arg_B remain TRANSFERRED (job still
 *     running on CPU1). result stays OWNED by CPU1 (never transferred back).
 *     Orphan cleanup (cpu1_orphan_jobs) transitions abandoned args to ORPHANED.
 *
 * Trust note: `fn` is a kernel function pointer chosen by the BSP (kernel code),
 * never user-supplied. CPU 1 runs ONLY what the BSP places here.
 */
int cpu1_submit(cpu_job_fn fn, void *arg)
{
    if (!fn) {
        return 0;
    }

    /* Check if CPU1 is offline (crashed/panicked). Reject new job submissions. */
    if (__atomic_load_n(&cpu1_offline, __ATOMIC_ACQUIRE)) {
        return 0;
    }

    /* CRITICAL SECTION: acquire lock before writing to cpu1_job slot. Multiple
     * concurrent submitters (e.g. syscall offloads from different processes) must
     * serialize here to avoid torn writes to fn/arg/owner_pid/ownership state. */
    spin_lock(&cpu1_job_lock);

    /* Publish the job. fn/arg first, track owner for orphan cleanup, then clear
     * done, then RELEASE pending. owner_pid lets process_unref orphan in-flight
     * jobs if the submitting process exits before the job completes. */
    cpu1_job.fn  = fn;
    cpu1_job.arg = arg;
    cpu1_job.owner_pid = current_process ? current_process->pid : 0;
    cpu1_job.job_seq++;  /* bump sequence to guard slot reuse before orphan drains */

    /* NORMALIZE the slot's ownership descriptors back to the OWNED birth state
     * before the per-job transition. This is REQUIRED for slot reuse after an
     * orphaned job: cpu1_orphan_jobs() leaves arg_A/arg_B/result in the terminal
     * ORPHANED state, and ORPHANED -> TRANSFERRED is illegal (own_transition()
     * would panic). own_init() re-births each descriptor to OWNED(owner_cpu=BSP,
     * refcount=1) from ANY prior state, so the OWNED -> TRANSFERRED edge below is
     * always legal. On the common path (previous job reclaimed cleanly, already
     * OWNED) this is a cheap idempotent reset. The previous job's buffers are a
     * SEPARATE allocation (each offload kmalloc_ref's its own), so re-initting the
     * descriptor never disturbs a still-draining orphan's lifetime -- that is
     * governed by the buffer's own kref, not this reusable slot token.
     *
     * We are inside cpu1_job_lock AND (for a fresh submit) pending was already 0,
     * so no concurrent waiter/orphaner is mid-transition on these descriptors. */
    own_init(&cpu1_job.arg_A);
    own_init(&cpu1_job.arg_B);
    own_init(&cpu1_job.result);

    /* OWNERSHIP STATE TRANSITION: OWNED (CPU0) -> TRANSFERRED (to CPU1).
     * After this point CPU0 must NOT access the arg buffers until the job
     * completes and cpu1_wait transitions them back. The AP worker loop will
     * observe pending==1 via ACQUIRE and consume the TRANSFERRED args.
     *
     * `result` is ALSO transitioned OWNED -> TRANSFERRED(to CPU1) here: CPU1 is
     * the PRODUCER of the result, so ownership moves to it on submit and moves
     * back to CPU0 (TRANSFERRED -> OWNED) only when cpu1_wait succeeds. This
     * keeps result symmetric with arg_A/arg_B and -- critically -- avoids the
     * illegal OWNED -> OWNED self-edge that own_transition() panics on (the slot
     * is born OWNED in cpu1_job_init, so the reclaim in cpu1_wait MUST be from
     * TRANSFERRED, not from OWNED). On timeout result stays TRANSFERRED, which is
     * the honest state: CPU1 may still be writing it. */
    own_transition(&cpu1_job.arg_A,  OWN_TRANSFERRED, 1);  /* to CPU1 */
    own_transition(&cpu1_job.arg_B,  OWN_TRANSFERRED, 1);  /* to CPU1 */
    own_transition(&cpu1_job.result, OWN_TRANSFERRED, 1);  /* to CPU1 (producer) */

    __atomic_store_n(&cpu1_job.done, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&cpu1_job.pending, 1, __ATOMIC_RELEASE);

    /* Release lock AFTER the RELEASE barrier publishes the job. CPU1 can now
     * observe pending==1 and consume the job safely. */
    spin_unlock(&cpu1_job_lock);

    __atomic_fetch_add(&cpu1_dispatch_total, 1, __ATOMIC_RELAXED);  /* observability only */
    return 1;                           /* submitted -- do NOT wait here */
}

/*
 * cpu1_diagnose_timeout -- enhanced diagnostics to distinguish slow vs wedged vs
 * panicked AP after a job timeout. Returns a diagnostic string (for logging).
 *
 * Strategy:
 *   1. Read CPU1's heartbeat counter before and after a short delay (~100 ms).
 *   2. If heartbeat advances: CPU1 is ALIVE but the job is slow (or looping).
 *   3. If heartbeat stuck: CPU1 is WEDGED (hung/dead, not servicing timer tick).
 *   4. Check cpu1_panic_flag: CPU1 hit panic, may be stuck in panic loop.
 *
 * This helps the operator or recovery logic decide what to do (e.g., trigger AP
 * recovery, increase timeout, or treat as hard failure).
 */
static const char* cpu1_diagnose_timeout(uint64_t timeout_ms)
{
    /* NOTE: Health monitor heartbeat checks disabled until percpu_data integrated.
     * For now, rely on cpu1_panic_flag for basic diagnostic. */

    /* Check if CPU1 panicked */
    bool cpu1_panicked = __atomic_load_n(&cpu1_panic_flag, __ATOMIC_ACQUIRE);

    /* Basic diagnostic based on panic flag only */
    if (cpu1_panicked) {
        /* CPU1 panicked (stuck in panic loop or halted) */
        kprintf("[OFFLOAD] Job timeout after %llu ms: CPU1 PANICKED\n",
                timeout_ms);
        return "panicked";
    } else {
        /* Timeout without panic: either slow or wedged (can't distinguish without heartbeat) */
        kprintf("[OFFLOAD] Job timeout after %llu ms: CPU1 timeout (slow or wedged)\n",
                timeout_ms);
        return "timeout";
    }
}

/*
 * cpu1_wait -- bounded wait for the in-flight job to finish.
 *
 * ACQUIRE-poll the shared MEMORY flag `done` until it is set or `deadline_tsc`
 * (an ABSOLUTE rdtsc value) passes. Returns 1 if the AP signalled done within the
 * deadline, 0 on timeout. Observing done==1 synchronizes-with the AP's RELEASE-store
 * of done, so all of fn's shared-memory side effects are visible before we return.
 *
 * The deadline is supplied by the caller so the SAME primitive serves BOTH a short
 * ~100ms LIVENESS bound (brick 6 self-test: a wedged AP must never hang us) AND a
 * GENEROUS multi-second compute bound (brick 8 matmul: real work takes real time).
 * It polls MEMORY only (never any MMIO/APIC register), so a wedged LAPIC can never
 * make the BSP spin forever -- the finite deadline always wins.
 *
 * OWNERSHIP INTEGRATION:
 *   - On SUCCESS: transitions arg_A/arg_B from TRANSFERRED -> OWNED (CPU0 reclaims),
 *     transitions result from TRANSFERRED -> OWNED (CPU0 takes ownership of result).
 *   - On TIMEOUT: leaves arg_A/arg_B in TRANSFERRED state (CPU1 still running the
 *     job, may still access them). orphan cleanup will transition them to ORPHANED
 *     if the process exits. result remains OWNED by CPU1 (never transferred back).
 *
 * TIMEOUT DIAGNOSTICS: On timeout, run enhanced diagnostics (cpu1_diagnose_timeout)
 * to distinguish between slow job, wedged AP, or panicked AP. This helps operators
 * and recovery logic understand what went wrong.
 */
int cpu1_wait(uint64_t deadline_tsc)
{
    uint64_t start_tsc = rdtsc();  /* Remember start time for diagnostics */

    while (__atomic_load_n(&cpu1_job.done, __ATOMIC_ACQUIRE) == 0) {
        /* Check if CPU1 panicked during job execution. */
        if (__atomic_load_n(&cpu1_panic_flag, __ATOMIC_ACQUIRE)) {
            kprintf("[OFFLOAD] CPU1 panicked during job execution\n");

            /* Collect crash report: AP already printed context via its panic handler. */
            kprintf("[OFFLOAD] CPU1 crash context:\n");
            kprintf("  Vector: %llu  Error: 0x%llx  RIP: 0x%016llx\n",
                    cpu1_crash_ctx.vector, cpu1_crash_ctx.error_code, cpu1_crash_ctx.rip);
            kprintf("  RSP: 0x%016llx  CR2: 0x%016llx  CR3: 0x%016llx\n",
                    cpu1_crash_ctx.rsp, cpu1_crash_ctx.cr2, cpu1_crash_ctx.cr3);

            /* Mark CPU1 offline so future job submissions know it's dead. */
            smp_cpu_offline(1);

            /* Return error to caller (system continues single-core, degraded but alive). */
            return EAPFAULT;            /* AP fault -> system degraded */
        }

        if (rdtsc() >= deadline_tsc) {
            /* Timeout: leave owner_pid set so orphan cleanup can detect and handle
             * this pending job if the process exits before it completes. Leave
             * arg_A/arg_B in TRANSFERRED state -- CPU1 may still be accessing them;
             * orphan cleanup will handle state transition to ORPHANED if needed. */

            /* Run enhanced diagnostics to distinguish slow vs wedged vs panicked */
            uint64_t elapsed_tsc = rdtsc() - start_tsc;
            uint64_t timeout_ms  = elapsed_tsc / (AP_TSC_PER_US * 1000ULL);
            cpu1_diagnose_timeout(timeout_ms);

            uint64_t to = __atomic_add_fetch(&cpu1_timeout_total, 1, __ATOMIC_RELAXED);
            kprintf("[SMP] dispatch=%llu cpu1_jobs=%llu lost_wakeups=%llu stuck_tasks=0 (timeout)\n",
                    (unsigned long long)__atomic_load_n(&cpu1_dispatch_total, __ATOMIC_RELAXED),
                    (unsigned long long)__atomic_load_n(&cpu1_complete_total, __ATOMIC_RELAXED),
                    (unsigned long long)to);
            return 0;                   /* timeout -> don't hang the BSP */
        }
        __asm__ volatile("pause" ::: "memory");
    }

    /* Job completed. OWNERSHIP STATE TRANSITIONS:
     *   arg_A, arg_B: TRANSFERRED (to CPU1) -> OWNED (reclaimed by CPU0).
     *   result: TRANSFERRED (from CPU1) -> OWNED (CPU0 takes ownership).
     * After these transitions CPU0 may safely read/write the args and result.
     *
     * CRITICAL: acquire cpu1_job_lock to serialize these state transitions against a
     * concurrent cpu1_orphan_jobs (which transitions TRANSFERRED -> ORPHANED under
     * the same lock). Without the lock, orphan cleanup racing a successful wait could
     * trigger concurrent state transitions and panic on an illegal edge. Same lock
     * that guards submit (OWNED -> TRANSFERRED) and orphan (TRANSFERRED -> ORPHANED),
     * so all three ownership transition sites are now serialized. */
    spin_lock(&cpu1_job_lock);
    own_transition(&cpu1_job.arg_A, OWN_OWNED, 0);  /* CPU0 reclaims */
    own_transition(&cpu1_job.arg_B, OWN_OWNED, 0);  /* CPU0 reclaims */
    own_transition(&cpu1_job.result, OWN_OWNED, 0); /* CPU0 takes result */

    /* Clear owner_pid so orphan cleanup knows there's nothing to clean up if the
     * process exits now. */
    cpu1_job.owner_pid = 0;
    spin_unlock(&cpu1_job_lock);

    /* Observability only: count the completion and, every 256th job, emit a running
     * counter line so a stress run leaves periodic [SMP] evidence in the boot log
     * without spamming it. dispatch should track complete 1:1 under a healthy mailbox;
     * a growing lost_wakeups (timeout) gap is the signal the proof harness watches. */
    uint64_t done_n = __atomic_add_fetch(&cpu1_complete_total, 1, __ATOMIC_RELAXED);
    if ((done_n & 0xFF) == 0) {
        kprintf("[SMP] dispatch=%llu cpu1_jobs=%llu lost_wakeups=%llu stuck_tasks=0\n",
                (unsigned long long)__atomic_load_n(&cpu1_dispatch_total, __ATOMIC_RELAXED),
                (unsigned long long)done_n,
                (unsigned long long)__atomic_load_n(&cpu1_timeout_total, __ATOMIC_RELAXED));
    }

    return 1;                           /* CPU 1 ran the job and signalled done */
}

/*
 * cpu1_run -- BSP-side submit + bounded wait (brick 6, UNCHANGED behaviour). Hand the
 * ONE trusted worker function `fn` (with `arg`) to CPU 1 via the single job slot, then
 * wait for it to finish. Now expressed as cpu1_submit() + cpu1_wait(default deadline).
 *
 * Returns 1 if CPU 1 ran the job and signalled done within the deadline, 0 on
 * timeout. The wait is a BOUNDED ~100 ms TSC deadline so a wedged/dead AP can NEVER
 * hang the BSP -- mirroring THE HARD RULE used everywhere else in this file. This is
 * the LIVENESS path (existing brick-6 sum self-test); compute jobs that legitimately
 * take longer use cpu1_submit + cpu1_wait with a generous deadline instead.
 *
 * Trust note: `fn` is a kernel function pointer chosen by the BSP (kernel code),
 * never user-supplied. CPU 1 runs ONLY what the BSP places here.
 */
int cpu1_run(cpu_job_fn fn, void *arg)
{
    if (!cpu1_submit(fn, arg)) {
        return 0;
    }
    /* BOUNDED ~100 ms TSC deadline (absolute). On expiry cpu1_wait returns 0 (the
     * AP is wedged/absent) so the BSP keeps going; never an infinite spin, never a
     * panic. */
    uint64_t deadline = rdtsc() + AP_WAIT_US * AP_TSC_PER_US;  /* now + 100ms */
    return cpu1_wait(deadline);
}

/*
 * cpu1_orphan_jobs -- called from process_unref when a process exits with a
 * pending CPU1 job. The exiting process must NOT block indefinitely (violates the
 * async goal) and must NOT free buffers CPU1 is still touching.
 *
 * TWO-PHASE RESOLUTION ("wait 100ms or orphan"):
 *   Phase 1 (GRACE WAIT): if our job is still in-flight, poll the slot for a
 *     BOUNDED ~100 ms (AP_WAIT_US). Most offloads finish in well under that, so the
 *     common case is a clean drain: the AP clears `pending`, the slot's ownership
 *     descriptors are reclaimed normally, and there is nothing to orphan. We poll
 *     MEMORY only (never MMIO) and on a finite TSC deadline, so a wedged AP can
 *     never hang the exiting process -- the deadline always wins.
 *   Phase 2 (ORPHAN): if the job is STILL pending after the grace period, orphan
 *     it. Under cpu1_job_lock (serializing against a concurrent cpu1_submit), if
 *     owner_pid still matches the exiting PID, clear owner_pid (the result is
 *     dropped on completion) and transition arg_A/arg_B/result TRANSFERRED ->
 *     ORPHANED. ORPHANED marks the buffers "owner gone, but CPU1 may still be
 *     accessing them" so they cannot be reused/refreshed until the next submit
 *     re-inits the slot. The offload BUFFERS themselves are kref'd (kmalloc_ref in
 *     sys_cpu1_offload); the handler's own kput drives them to free only once CPU1
 *     is also done, so there is no UAF and no leak. job_seq guards slot reuse by a
 *     newer job before the orphaned one drains.
 *
 * Either way the exiting process returns promptly (<= ~100 ms, usually instantly).
 *
 * OWNERSHIP INTEGRATION:
 *   - arg_A/arg_B/result: TRANSFERRED (to CPU1) -> ORPHANED (owner exited, CPU1
 *     still running). The AP worker loop eventually finishes; the descriptors stay
 *     ORPHANED (terminal) until the NEXT cpu1_submit re-inits the slot via
 *     own_init(). The actual buffers are reclaimed via their kref when both the
 *     handler and CPU1 have released.
 *
 * Edge case: if the job finished before/at any point (pending==0), we do nothing
 * (already cleaned by the submit/wait path).
 */
void cpu1_orphan_jobs(uint32_t exiting_pid)
{
    /* ACQUIRE-load pending to see if a job is in-flight. If pending==0, the job
     * already completed or was never submitted; nothing to orphan. */
    if (__atomic_load_n(&cpu1_job.pending, __ATOMIC_ACQUIRE) == 0) {
        return;  /* slot is free; no orphan cleanup needed */
    }

    /* Only the OWNER of the in-flight job needs to grace-wait/orphan. A different
     * process's pending job is none of our business -- bail without spinning so an
     * unrelated exit never burns 100ms. (owner_pid is a single aligned 32-bit word;
     * a torn read here at worst causes a spurious early return, which is safe -- the
     * locked re-check below is authoritative for the actual orphan.) */
    if (cpu1_job.owner_pid != exiting_pid) {
        return;
    }

    /* Snapshot job_seq so we can tell, after the grace wait, whether the slot was
     * recycled by a NEWER job in the meantime (the AP finished ours, a new submit
     * bumped job_seq). If it changed, OUR job already drained -- do not orphan the
     * unrelated successor. */
    uint64_t my_seq = __atomic_load_n(&cpu1_job.job_seq, __ATOMIC_ACQUIRE);

    /* PHASE 1 -- GRACE WAIT: give the in-flight job up to ~100 ms (AP_WAIT_US) to
     * finish cleanly. Poll MEMORY only on a finite TSC deadline (a wedged AP can
     * never hang us). If the job drains (pending->0) or the slot is recycled
     * (job_seq moves), our work is done and there is nothing to orphan. */
    uint64_t deadline = rdtsc() + AP_WAIT_US * AP_TSC_PER_US;  /* now + 100 ms */
    while (__atomic_load_n(&cpu1_job.pending, __ATOMIC_ACQUIRE) != 0 &&
           __atomic_load_n(&cpu1_job.job_seq, __ATOMIC_ACQUIRE) == my_seq) {
        if (rdtsc() >= deadline) {
            break;                       /* grace expired -> fall through to orphan */
        }
        __asm__ volatile("pause" ::: "memory");
    }

    /* Take cpu1_job_lock so the owner_pid read+clear and the ownership orphan
     * transitions are serialized against a concurrent cpu1_submit() on another
     * CPU0 (which writes owner_pid/job_seq and the same ownership descriptors
     * under this same lock). Without it, a submit racing an exit could observe a
     * half-updated slot or orphan a buffer a brand-new job just transferred. */
    spin_lock(&cpu1_job_lock);

    /* Re-check pending AND job_seq under the lock: the job may have completed (the
     * AP cleared pending) or the slot may have been recycled by a newer job during
     * the grace wait. Either way OUR job is gone -- nothing to orphan. */
    if (__atomic_load_n(&cpu1_job.pending, __ATOMIC_ACQUIRE) == 0 ||
        cpu1_job.job_seq != my_seq) {
        spin_unlock(&cpu1_job_lock);
        return;
    }

    /* PHASE 2 -- ORPHAN: the job is STILL pending and STILL ours after the grace
     * period. Orphan it. The AP will finish, but the result will be silently
     * dropped (owner_pid==0). */
    if (cpu1_job.owner_pid == exiting_pid) {
        /* Clear owner_pid so the completion path knows this job is orphaned. */
        cpu1_job.owner_pid = 0;

        /* OWNERSHIP STATE TRANSITION: TRANSFERRED -> ORPHANED for arg_A/arg_B/
         * result. All three were moved to TRANSFERRED(to CPU1) in cpu1_submit and
         * were NOT reclaimed (the waiter timed out or the process exited before
         * waiting). Marking them ORPHANED says "owner gone, but CPU1 may still be
         * touching them" so they cannot be freed until CPU1 finishes and releases
         * its refs. ORPHANED is terminal: no new owner can be established, which
         * is exactly what we want for a drained-but-not-yet-reclaimed buffer.
         * TRANSFERRED -> ORPHANED is a legal edge in OWN_TRANSITION_TABLE. */
        own_orphan(&cpu1_job.arg_A);
        own_orphan(&cpu1_job.arg_B);
        own_orphan(&cpu1_job.result);

        /* The job_seq remains unchanged; it will be bumped when the next job is
         * submitted (after this orphaned one drains), preventing slot reuse races. */
    }

    spin_unlock(&cpu1_job_lock);

    /* Return immediately; do NOT wait for the job to finish. The AP will complete
     * it asynchronously, the result will be discarded, and the slot will be freed
     * normally via the pending RELEASE-store in ap_main. */
}

/* ---------------------------------------------------------------------------
 * SMP brick 8: split an INTEGER matrix multiply across CPU0 (BSP) and CPU1 (AP)
 * and measure the real speedup -- the payoff of the SMP arc. The BSP computes the
 * top band of C while CPU1 computes the bottom band CONCURRENTLY (via cpu1_submit
 * + own work + cpu1_wait), then the BSP verifies the dual-core result bit-for-bit
 * against a single-core baseline. CPU1 records its own APIC id into the job arg so
 * the log can PROVE apic-1 ran the bottom band (not a silent serial fallback on CPU0).
 *
 * WHY INTEGER (no float/double/SSE anywhere on the AP path): the AP trampoline does
 * not provably enable the SSE FPU state (CR0/CR4.OSFXSR) on CPU1, so float/SSE code
 * on the AP could #UD or compute garbage. Integer math uses GP registers, always
 * valid in long mode. matmul_band and everything it touches are int-only, so the
 * compiler emits NO SSE for the AP path. (Float/SSE on the AP is a deferred brick.)
 *
 * Buffers are static, in the BSP's page tables (so already mapped + reachable from
 * the AP). A/B are filled deterministically with SMALL ints so the result is
 * reproducible and bit-identical between the single- and dual-core runs, and the
 * int64 accumulator cannot overflow (per-element sum <= N*6*4 ~ 3072, trivially safe).
 *
 * .bss.deferred PLACEMENT (load-bearing -- see kernel/linker.ld): these four arrays
 * total ~384 KiB. Plain .bss is packed in the first ~1.7 MiB, which MUST stay BELOW
 * the 0x200000 user-ELF load base: each process address space deep-copies the
 * low-half page tables and userspace binaries link at 0x200000, so any kernel global
 * at VA >= 0x200000 gets SHADOWED by user pages under that process's CR3. 384 KiB of
 * extra plain .bss pushes the kernel's critical control state across 0x200000 and the
 * first usermode entry (PID 2) then GPFs. Emitting these LAST in .bss.deferred (which
 * the linker places after all control state) keeps control state packed below
 * 0x200000. Safe for these buffers specifically: the whole matmul self-test runs in
 * kernel_main BEFORE any userspace process exists (kernel CR3 active, buffers fully
 * mapped) and they are never touched again afterward -- so even if a later big process
 * shadows them, nothing reads them. NOT volatile/touched on any user-CR3 hot path.
 * ------------------------------------------------------------------------- */
#define MM_N 128                        /* square: C[N][N] = A[N][K=N] * B[N][N] */
#define MM_DEFERRED __attribute__((section(".bss.deferred")))
static int32_t mm_A[MM_N * MM_N] MM_DEFERRED;       /* row-major */
static int32_t mm_B[MM_N * MM_N] MM_DEFERRED;
static int64_t mm_Csingle[MM_N * MM_N] MM_DEFERRED; /* single-core baseline result */
static int64_t mm_Cdual[MM_N * MM_N] MM_DEFERRED;   /* dual-core (BSP top + CPU1 bottom) */

/* Fill A/B deterministically with small ints (reproducible, bit-identical runs). */
void mm_fill(void)
{
    for (int i = 0; i < MM_N; i++) {
        for (int j = 0; j < MM_N; j++) {
            mm_A[i * MM_N + j] = (int32_t)((i + j) % 7);
            mm_B[i * MM_N + j] = (int32_t)((i * 3 + j + 1) % 5);
        }
    }
}

/* Compute C rows [row0,row1). INT-ONLY (GP registers): no float/double/SSE, so this
 * is safe to run on the AP, which has no proven SSE state. C is the caller's buffer
 * (mm_Csingle for the baseline, mm_Cdual for each parallel band). */
static void matmul_band(int row0, int row1, int64_t *C)
{
    for (int r = row0; r < row1; r++) {
        for (int c = 0; c < MM_N; c++) {
            int64_t s = 0;
            for (int k = 0; k < MM_N; k++) {
                s += (int64_t)mm_A[r * MM_N + k] * (int64_t)mm_B[k * MM_N + c];
            }
            C[r * MM_N + c] = s;
        }
    }
}

/* CPU1's band job + proof-of-execution marker. mm_job runs the bottom band on the AP
 * and records the AP's own APIC id (lapic_get_id() -- the same id source the rest of
 * the SMP code uses) into mm_arg.by_apic, so the BSP log can PROVE apic-1 executed
 * the band. by_apic is volatile so the store is not elided; cross-core visibility of
 * both the band results AND by_apic is provided by the cpu1_job.done RELEASE/ACQUIRE
 * pair (mm_job runs INSIDE the AP worker loop's job dispatch, before it sets done). */
static struct {
    int           row0;
    int           row1;
    int64_t      *C;
    volatile int  by_apic;
} mm_arg;

static void mm_job(void *a)
{
    (void)a;
    matmul_band(mm_arg.row0, mm_arg.row1, mm_arg.C);
    mm_arg.by_apic = (int)lapic_get_id();   /* PROOF: which CPU ran this band */
}

/*
 * matmul_self_test -- the brick-8 dual-core self-test, driven by the BSP. Called from
 * kernel.c AFTER the brick-6 sum test and AFTER CPU 1 is confirmed online.
 *
 *   1. Fill A/B deterministically.
 *   2. SINGLE-CORE baseline: time matmul_band(0,N) on the BSP -> single_cycles.
 *   3. DUAL-CORE: cpu1_submit(mm_job) for the BOTTOM half [N/2,N), then the BSP
 *      computes the TOP half [0,N/2) CONCURRENTLY, then cpu1_wait(generous deadline).
 *      -> dual_cycles. A generous ~5s TSC deadline (NOT the 100ms liveness bound) is
 *      used because this is REAL work; on timeout we log a clear FAIL and skip the
 *      speedup line rather than hang.
 *   4. CORRECTNESS: compare mm_Cdual vs mm_Csingle element-by-element.
 *   5. LOG (kprintf, same logger as the surrounding SMP code): the split + by_apic
 *      proof, raw single/dual cycles (%lu, 64-bit), an INTEGER speedup (x.xx), and
 *      the verify result.
 *
 * INT-ONLY throughout (the speedup is computed with integer math; no float anywhere),
 * so nothing on the AP path emits SSE.
 */
void matmul_self_test(void)
{
    /* 1. Deterministic inputs. */
    mm_fill();

    /* 2. Single-core baseline (whole matrix on the BSP). */
    uint64_t t0 = rdtsc();
    matmul_band(0, MM_N, mm_Csingle);
    uint64_t t1 = rdtsc();
    uint64_t single_cycles = t1 - t0;

    /* 3. Dual-core: CPU1 takes the bottom band, the BSP takes the top band, in
     *    parallel. mm_arg.by_apic starts 0; mm_job sets it to the AP's apic id. */
    mm_arg.row0    = MM_N / 2;
    mm_arg.row1    = MM_N;
    mm_arg.C       = mm_Cdual;
    mm_arg.by_apic = 0;

    t0 = rdtsc();
    int submitted = cpu1_submit(mm_job, &mm_arg);   /* CPU1: bottom half, async   */
    matmul_band(0, MM_N / 2, mm_Cdual);             /* BSP : top half, concurrent */
    /* GENEROUS compute deadline (~5s of TSC at the ~3 GHz convention) -- this is
     * real work, not a liveness probe. On timeout: clear FAIL, no hang. */
    uint64_t deadline = rdtsc() + 5ULL * 1000000ULL * AP_TSC_PER_US;
    int finished = submitted && cpu1_wait(deadline);
    t1 = rdtsc();
    uint64_t dual_cycles = t1 - t0;

    if (!finished) {
        kprintf("[SMP] matmul DUAL-CORE job did not finish within deadline "
                "(submitted=%d) -- FAIL, skipping speedup\n", submitted);
        kprintf("[SMP] matmul verify: FAIL (CPU1 band did not complete)\n");
        return;
    }

    /* 4. Correctness: dual must equal single, element-for-element. */
    int mismatches = 0;
    for (int i = 0; i < MM_N * MM_N; i++) {
        if (mm_Cdual[i] != mm_Csingle[i]) {
            mismatches++;
        }
    }

    /* 5. Log the split (with the by_apic PROOF), the raw cycle counts, the integer
     *    speedup, and the verify result. */
    kprintf("[SMP] matmul %dx%d split CPU0[0..%d) + CPU1[%d..%d) by_apic=%d\n",
            MM_N, MM_N, MM_N / 2, MM_N / 2, MM_N, mm_arg.by_apic);
    kprintf("[SMP] matmul single=%lu cycles dual=%lu cycles\n",
            single_cycles, dual_cycles);

    /* Integer speedup x100 (no float): single/dual to two decimals. Guard div-by-0. */
    if (dual_cycles > 0) {
        uint64_t speedup_x100 = (single_cycles * 100ULL) / dual_cycles;
        kprintf("[SMP] matmul speedup=%lu.%02lux\n",
                speedup_x100 / 100ULL, speedup_x100 % 100ULL);
    } else {
        kprintf("[SMP] matmul speedup=n/a (dual_cycles==0)\n");
    }

    if (mismatches == 0) {
        kprintf("[SMP] matmul verify: OK\n");
    } else {
        kprintf("[SMP] matmul verify: FAIL (%d mismatches)\n", mismatches);
    }
}

/* ===========================================================================
 * USERSPACE -> CPU1 OFFLOAD PATH (the userspace bridge).
 *
 * SYS_CPU1_OFFLOAD (handlers.c, gated by SMP_FOUNDATION) lets a NORMAL ring-3
 * process ask CPU1 to run a kernel matmul on COPIED data and return the result.
 * The handler does ALL user-memory validation + copy-in/out with the kernel's
 * safe copy_from_user/copy_to_user helpers; by the time control reaches THIS file
 * the data is already in trusted kernel buffers. CPU1 therefore runs ONLY the
 * trusted kernel matmul below on kernel-owned memory -- NO user pointer, NO user
 * code, NO scheduler, NO migration ever reaches the AP. It stays a pure trusted
 * coprocessor: the BSP publishes one kernel fn into the single job slot (cpu1_run)
 * and CPU1 executes exactly that, mirroring the brick-6/8 self-tests.
 *
 * BUFFER PLACEMENT (load-bearing -- why NOT .bss.deferred): the offload runs on
 * demand from a syscall handler while a USER process is current, i.e. with that
 * process's CR3 active. The boot self-test's mm_A/mm_B/mm_Cdual live in
 * .bss.deferred, which can be SHADOWED by user pages once a process maps memory at
 * VA >= 0x200000 (see the mm_* placement note above) -- reading/writing them under
 * a user CR3 would touch the USER's pages, not the kernel result (this manifested
 * as the first page of the result being wrong). So the offload uses NO static
 * buffers at all: the handler hands us kmalloc'd KERNEL-HEAP buffers (heap base
 * 0xFFFFFFFF90000000, high-half canonical kernel space that is NEVER shadowed by
 * the low-half user mappings), and CPU1 -- running in the BSP's kernel CR3 -- reads
 * the operands and writes the result directly into those heap buffers. The handler
 * then reads them back under the user CR3 safely (high-half, unshadowed).
 *
 * SINGLE offload at a time: the one cpu1_job slot serializes dispatch, but each
 * offload carries its OWN heap buffers (in the job arg below), so there are no
 * shared result buffers to collide -- concurrent offloads are a later refinement
 * (the slot itself would need to become multi-entry); for now one-at-a-time.
 * ------------------------------------------------------------------------- */

/* Offload job arg: the operand/result KERNEL-HEAP pointers + dimension + the AP's
 * proof-of-execution apic id. A/B/C point at kmalloc'd high-half kernel memory the
 * handler owns (NOT .bss.deferred), so CPU1 (kernel CR3) and the handler (user CR3)
 * both address them safely. by_apic is volatile so mm_offload_job's store is not
 * elided; cross-core visibility of BOTH the result AND by_apic is provided by the
 * cpu1_job.done RELEASE/ACQUIRE pair (the job runs inside the AP worker loop's
 * dispatch, before it sets done).
 *
 * refA/refB/refC are the kref'd payload pointers for CPU1's async lifetime ref.
 * refs_held is a one-shot guard ensuring exactly-once release. */
static struct {
    const int32_t *A;
    const int32_t *B;
    int64_t       *C;
    int            n;
    volatile int   by_apic;
    void          *refA;        /* kref'd payload for kget/kput (same addr as A) */
    void          *refB;        /* kref'd payload for kget/kput (same addr as B) */
    void          *refC;        /* kref'd payload for kget/kput (same addr as C) */
    volatile int   refs_held;   /* 1 after kget, 0 after the single kput */
} mm_off_arg;

/* INT-ONLY square matmul of dimension n (row stride n), rows [row0,row1). A
 * dimension-parameterized sibling of matmul_band (which is hard-wired to the
 * MM_N stride for the boot self-test and is left BYTE-IDENTICAL). GP-register
 * integer math only -- no float/SSE -- so it is safe to run on the AP, which has
 * no proven SSE state. */
static void matmul_band_n(int row0, int row1, int n,
                          const int32_t *A, const int32_t *B, int64_t *C)
{
    for (int r = row0; r < row1; r++) {
        for (int c = 0; c < n; c++) {
            int64_t s = 0;
            for (int k = 0; k < n; k++) {
                s += (int64_t)A[r * n + k] * (int64_t)B[k * n + c];
            }
            C[r * n + c] = s;
        }
    }
}

/* Release CPU1's async ref on the offload buffers exactly once (kput refA/B/C).
 * Called at the END of mm_offload_job (after the last read of A/B and last write
 * of C, after by_apic is stored). The atomic exchange ensures exactly-once even
 * if (future) another path tries to release. This is the buffer-free analogue of
 * the AP worker loop's done/pending stores, and it runs INSIDE the fn, i.e. BEFORE
 * ap_main sets done -- so CPU1 is provably finished with the buffers before CPU0's
 * cpu1_wait can observe completion. */
static void mm_offload_release(void) {
    if (__atomic_exchange_n(&mm_off_arg.refs_held, 0, __ATOMIC_ACQ_REL)) {
        kput(mm_off_arg.refA);
        kput(mm_off_arg.refB);
        kput(mm_off_arg.refC);
    }
}

/* The trusted kernel fn CPU1 runs for an offload: compute the WHOLE matrix [0,n)
 * into the handler's kmalloc'd C from the handler's kmalloc'd A/B, then record the
 * AP's own apic id so the BSP (and the requesting app) can PROVE apic-1 executed
 * it. Pure int math on kernel-heap buffers -- nothing user-supplied. */
static void mm_offload_job(void *a)
{
    (void)a;
    matmul_band_n(0, mm_off_arg.n, mm_off_arg.n,
                  mm_off_arg.A, mm_off_arg.B, mm_off_arg.C);
    mm_off_arg.by_apic = (int)lapic_get_id();   /* PROOF: which CPU ran it */
    mm_offload_release();                       /* Drop CPU1's async ref */
}

/*
 * cpu1_offload_matmul -- BSP-side driver for a userspace matmul offload.
 *
 * A, B and C_out are KERNEL-HEAP buffers (the syscall handler kmalloc'd them,
 * validated + copied the user operands into A/B, and will copy C_out back out).
 * High-half kernel heap is NOT shadowed by user mappings, so it is safe to address
 * under any CR3 -- crucially CPU1 writes C_out directly. This function publishes
 * the WHOLE matmul as ONE trusted job to CPU1 (cpu1_submit) with these pointers in
 * the job arg, then waits a GENEROUS bounded deadline (real compute, not a liveness
 * probe -> ~2s of TSC). The bound guarantees a wedged/absent AP can never hang the
 * calling process -- on timeout it returns 0 and the handler maps that to a
 * negative errno.
 *
 * KREF DISCIPLINE: Takes CPU1's own kref on each buffer (kget) BEFORE submit, so
 * each buffer's refcount = (1 if handler holds) + (1 if CPU1 async). On SUCCESS:
 * CPU1 releases first (mm_offload_release), then handler (1->0, free). On TIMEOUT:
 * handler's kput drives 2->1 (NOT freed), CPU1's later release drives 1->0 (real
 * free, after CPU1 provably done). No UAF, no leak, no double-free.
 *
 * Returns 1 and sets *by_apic_out (the apic id that ran the job, must be 1) on
 * success; 0 if CPU1 did not finish within the deadline (or args are bad).
 *
 * Trust: the fn handed to CPU1 (mm_offload_job) is a KERNEL function; CPU1 runs
 * only it, on the kernel-owned heap buffers. No user pointer/code reaches the AP.
 */
int cpu1_offload_matmul(const int32_t *A, const int32_t *B, int n,
                        int64_t *C_out, int *by_apic_out)
{
    if (n <= 0 || n > MM_N || !A || !B || !C_out) {
        return 0;
    }

    /* Publish the operand/result pointers + dimension for CPU1. CPU1 reads/writes
     * the handler's kmalloc'd kernel-heap buffers directly (no copy into static
     * .bss, which would be user-CR3-shadow-unsafe at offload time). */
    mm_off_arg.A       = A;
    mm_off_arg.B       = B;
    mm_off_arg.C       = C_out;
    mm_off_arg.n       = n;
    mm_off_arg.by_apic = 0;

    /* Take CPU1's async ref on each buffer BEFORE submit. The handler's own kref
     * on each buffer is still held (refcount >= 1), so kget validates the canary
     * and bumps 1->2 (or N->N+1). If any kget fails (corrupted magic), abort and
     * release what we got -- the handler will clean its own refs. */
    void *gA = kget((void*)A);
    void *gB = kget((void*)B);
    void *gC = kget((void*)C_out);
    if (!gA || !gB || !gC) {
        if (gA) kput(gA);
        if (gB) kput(gB);
        if (gC) kput(gC);
        return 0;                       /* handler maps 0 -> EAGAIN */
    }
    mm_off_arg.refA = gA;
    mm_off_arg.refB = gB;
    mm_off_arg.refC = gC;
    mm_off_arg.refs_held = 1;

    /* Publish the WHOLE matmul as one trusted job to CPU1, then wait a GENEROUS
     * bounded deadline (~2s at the ~3 GHz TSC convention). On timeout cpu1_wait
     * returns 0 and we bail -- never an infinite spin, never a hang. The cpu1_submit
     * RELEASE publishes mm_off_arg (incl. the pointers + the fresh refs) so the AP's
     * ACQUIRE sees them; the cpu1_wait ACQUIRE on `done` makes CPU1's writes to
     * C_out + by_apic visible here before we return. */
    if (!cpu1_submit(mm_offload_job, &mm_off_arg)) {
        /* Submit failed (CPU1 busy) -- drop the refs we just took, handler cleans
         * its own. refs_held is still 1, so mm_offload_release won't fire on CPU1. */
        kput(gA); kput(gB); kput(gC);
        mm_off_arg.refs_held = 0;
        return 0;
    }
    uint64_t deadline = rdtsc() + 2ULL * 1000000ULL * AP_TSC_PER_US;  /* now + 2s */
    if (!cpu1_wait(deadline)) {
        /* TIMEOUT: CPU1 is still running (or wedged). We do NOT drop CPU1's refs
         * here -- CPU1 holds them for its async lifetime and will release them in
         * mm_offload_release when (if) it finishes. The handler's kput will drive
         * refcount 2->1 (not freed); CPU1's later release drives 1->0 (real free,
         * after CPU1 is provably done). No UAF, no leak, no double-free. */
        return 0;                       /* AP wedged/absent -> caller returns <0 */
    }

    if (by_apic_out) {
        *by_apic_out = mm_off_arg.by_apic;
    }
    return 1;
}

/* Stage the trampoline blob + the shared param fields into low memory. */
static void ap_setup_trampoline(uint64_t kernel_cr3)
{
    size_t sz = (size_t)(ap_trampoline_end - ap_trampoline_start);
    memcpy((void *)AP_TRAMPOLINE_BASE, ap_trampoline_start, sz);

    uint8_t *param = (uint8_t *)(AP_TRAMPOLINE_BASE + AP_PARAM_OFFSET);

    /* CR3 (shared page tables) + 64-bit entry (ap_main) + the AP's stack top. */
    *(volatile uint64_t *)(param + AP_PARAM_CR3)       = kernel_cr3;
    *(volatile uint64_t *)(param + AP_PARAM_ENTRY)     = (uint64_t)&ap_main;
    *(volatile uint64_t *)(param + AP_PARAM_STACK_TOP) =
        (uint64_t)ap_stack + AP_STACK_SIZE;   /* top, grows down */
    *(volatile uint64_t *)(param + AP_PARAM_ARG)       = 1;   /* logical cpu 1 */

    /* Snapshot the BSP's live GDTR/IDTR so the AP loads the exact same tables.
     * (Sharing the BSP IDT is fine: the AP runs with interrupts masked and never
     * takes one -- it cli;hlts.) */
    dtr_image_t gdtr, idtr;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));
    __asm__ volatile("sidt %0" : "=m"(idtr));
    memcpy(param + AP_PARAM_GDTR, &gdtr, sizeof(gdtr));
    memcpy(param + AP_PARAM_IDTR, &idtr, sizeof(idtr));
}

/*
 * cpu1_job_init -- initialize ownership tracking for cpu1_job args and result.
 *
 * Called ONCE at boot before try_start_cpu1(). Initializes arg_A, arg_B, and
 * result ownership descriptors to OWNED state (owner_cpu=0, refcount=1). This
 * must be called BEFORE any cpu1_submit() so the ownership state transitions
 * have a valid starting point. Zero overhead: just three own_init() calls on
 * static .bss fields that start as all-zeroes.
 */
void cpu1_job_init(void)
{
    own_init(&cpu1_job.arg_A);    /* OWNED by CPU0, refcount=1 */
    own_init(&cpu1_job.arg_B);    /* OWNED by CPU0, refcount=1 */
    own_init(&cpu1_job.result);   /* OWNED by CPU0, refcount=1 (will be TRANSFERRED from CPU1) */
}

/*
 * try_start_cpu1 -- the owner's exact control flow for brick 3.
 *
 * Stage the trampoline, send INIT-SIPI-SIPI to CPU 1, then wait with a BOUNDED
 * TSC-deadline timeout polling the shared MEMORY flag ap1_online. Returns 1 if
 * CPU 1 came online within the deadline, 0 on timeout. NEVER blocks the BSP
 * indefinitely; NEVER panics. The caller logs + continues either way.
 *
 * SMP_FORCE_AP_FAIL (temporary, compile-time): target a deliberately bogus APIC
 * id so the AP never starts, to PROVE the BSP degrades safely (timeout ->
 * continue single-core). Off by default.
 */
int try_start_cpu1(void)
{
    /* CPU 1 = the second enabled processor in the MADT (index 1). */
    int ap_apic = madt_get_apic_id(1);
    if (ap_apic < 0) {
        kprintf("[SMP] no AP APIC id in MADT; staying single-core\n");
        return 0;
    }
    uint32_t aid = (uint32_t)ap_apic;

#ifdef SMP_SCHED
    /* Brick A: publish CPU1's hardware xAPIC id so cpu_id() can map it -> logical
     * 1. RELEASE so the AP (once it runs cpu_id() in ap_main) observes the value.
     * Captured here in the BSP from the MADT, before the SIPI. */
    __atomic_store_n(&g_ap1_apic_id, aid, __ATOMIC_RELEASE);
#endif

#ifdef SMP_FORCE_AP_FAIL
    /* Forced-failure mode: send the SIPI to a bogus APIC id (0xFE) that no CPU
     * answers, so ap1_online is never set and the bounded wait must expire. This
     * exercises THE hard rule (BSP continues on AP failure). */
    aid = 0xFE;
    kprintf("[SMP] SMP_FORCE_AP_FAIL: targeting bogus APIC id 0x%x\n", aid);
#endif

    uint64_t kernel_cr3 = read_cr3();
    ap_setup_trampoline(kernel_cr3);

    /* Arm the handshake flag BEFORE the first SIPI (publish the zero). */
    __atomic_store_n(&ap1_online, 0u, __ATOMIC_SEQ_CST);

    kprintf("[SMP] starting CPU 1 (APIC id %u) via INIT-SIPI-SIPI...\n", aid);

    /* INIT-SIPI-SIPI (Intel MP startup protocol). ICR writes = MMIO (brick 1). */
    lapic_send_init(aid);
    ap_tsc_delay_us(10000);             /* >= 10 ms INIT settle */

    lapic_send_startup(aid, AP_SIPI_VECTOR);
    ap_tsc_delay_us(200);               /* >= 200 us before the 2nd SIPI */

    /* Second SIPI only if the AP has not already flagged in. */
    if (__atomic_load_n(&ap1_online, __ATOMIC_ACQUIRE) == 0) {
        lapic_send_startup(aid, AP_SIPI_VECTOR);
    }

    /* BOUNDED wait: a TSC deadline ~100 ms out, polling the shared MEMORY flag.
     * This is the safe-degradation core -- it is a finite deadline, it polls
     * memory (NOT any MMIO/APIC register), and on expiry we fall through and
     * return 0 so the BSP continues. */
    uint64_t start    = rdtsc();
    uint64_t deadline = AP_WAIT_US * AP_TSC_PER_US;   /* 100ms * 3000 cyc/us */
    while (__atomic_load_n(&ap1_online, __ATOMIC_ACQUIRE) == 0) {
        if ((rdtsc() - start) >= deadline) {
            return 0;                   /* timeout -> degrade safely */
        }
        __asm__ volatile("pause" ::: "memory");
    }

    return 1;                           /* CPU 1 set its flag -> online */
}

/*
 * cpu1_is_online -- check if CPU1 is currently online and accepting jobs.
 *
 * Returns 1 if CPU1 is online and available for job submissions, 0 if it's
 * offline (failed to start, crashed, or panicked). Callers can use this to
 * decide whether to attempt offloading work to CPU1 or run it on the BSP.
 */
int cpu1_is_online(void)
{
    return (__atomic_load_n(&ap1_online, __ATOMIC_ACQUIRE) != 0 &&
            __atomic_load_n(&cpu1_offline, __ATOMIC_ACQUIRE) == 0);
}

/* ============================================================================
 * SMP Infrastructure for Health Monitor
 * ============================================================================
 * The health monitor needs cpu_data() and smp_num_online, which are normally
 * defined in smp.c. Since the SMP_FOUNDATION build doesn't include smp.c,
 * we provide minimal definitions here to support health monitoring.
 */

/* Per-CPU data array (normally from smp.c) */
percpu_data_t percpu_data[MAX_CPUS];

/* Number of online CPUs (normally from smp.c) */
uint32_t smp_num_online = 2;  /* BSP + AP1 for SMP_FOUNDATION */

/* Get per-CPU data pointer (normally from smp.c) */
percpu_data_t* cpu_data(uint32_t cpu) {
    if (cpu >= MAX_CPUS) return (void*)0;
    return &percpu_data[cpu];
}
