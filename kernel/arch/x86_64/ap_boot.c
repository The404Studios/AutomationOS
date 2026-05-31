/*
 * ap_boot.c -- SMP brick 3: bring ONE application processor (CPU 1) online as a
 * bare HEARTBEAT, then park it forever. GATED behind -DSMP_FOUNDATION (SMP=1).
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
 *   - The AP marks itself online, then (brick 3.5) loops forever incrementing
 *     ONLY its own isolated per-CPU heartbeat counter (cpu_hb[1]). No interrupts,
 *     no hlt, no shared state -- a pure spin that proves it runs independently.
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
 * always 1). Keep this MINIMAL: mark online, then run its private heartbeat.
 *
 * Intentionally does NOT log (two CPUs racing on the 0x3F8 serial port is not
 * MP-safe) -- the BSP prints "CPU 1 online" after it observes the flag. Does NOT
 * touch the scheduler, allocator, or any stateful subsystem. Does NOT enable
 * interrupts (no sti, no hlt, no timer, no handler).
 *
 * BRICK 3.5: after publishing online (SEQ_CST so the BSP's ACQUIRE load sees it),
 * the AP loops FOREVER incrementing ONLY its own counter, cpu_hb[1].v -- nothing
 * shared, nothing stateful. `pause` is a spin-loop hint (no scheduling, no IPI).
 * This active spin is the PROOF the AP is independently executing: while the BSP
 * runs its bounded ~4s proof window (and also paints the framebuffer), this core
 * is doing nothing but climbing cpu_hb[1], so cpu_hb[1] >> cpu_hb[0] -- that
 * asymmetry alone shows the AP ran on its own. A properly idling AP is a later
 * brick; here it deliberately never halts.
 */
void ap_main(uint64_t cpu)
{
    (void)cpu;

    /* Tell the BSP we reached long mode and are alive. SEQ_CST publish. */
    __atomic_store_n(&ap1_online, 1u, __ATOMIC_SEQ_CST);

    /* Heartbeat forever on the AP's OWN isolated cache line. No interrupts, no
     * hlt, no shared state -- just prove this core is executing independently. */
    for (;;) {
        cpu_hb[1].v++;
        __asm__ volatile("pause");
    }
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
