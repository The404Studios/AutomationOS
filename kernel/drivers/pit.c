#include "../include/x86_64.h"
#include "../include/kernel.h"
#include "../include/drivers.h"
#include "../include/sched.h"

// PIT I/O ports
#define PIT_CHANNEL0 0x40
#define PIT_CHANNEL1 0x41
#define PIT_CHANNEL2 0x42
#define PIT_COMMAND  0x43

// PIT command register bits
#define PIT_CMD_BINARY       0x00  // Use binary counter values
#define PIT_CMD_MODE2        0x04  // Mode 2: Rate Generator
#define PIT_CMD_MODE3        0x06  // Mode 3: Square Wave Generator
#define PIT_CMD_RW_BOTH      0x30  // Read/Write LSB then MSB
#define PIT_CMD_CHANNEL0     0x00  // Select channel 0

// PIT base frequency (1.193182 MHz)
#define PIT_BASE_FREQUENCY 1193182UL

// Tick counter
static volatile uint64_t timer_ticks = 0;

/*
 * timer_frequency stores the ACTUAL ticks-per-second derived from the divisor
 * we programmed into the PIT, not the caller-requested frequency.  This keeps
 * timer_get_ticks_ms() and timer_sleep() consistent even when the requested
 * frequency doesn't divide PIT_BASE_FREQUENCY evenly or when the divisor had
 * to be clamped to the hardware maximum of 65535.
 *
 * FIX-PIT-1: Using actual frequency avoids a systematic drift of up to one
 * tick per second when PIT_BASE_FREQUENCY % frequency != 0.
 */
static uint32_t timer_frequency = 0;

/*
 * pit_rflags_if -- return non-zero if the Interrupt Flag (IF) is set in
 * the current RFLAGS.  Used by timer_sleep() to decide whether hlt is safe.
 */
static inline uint64_t pit_rflags_if(void)
{
    uint64_t rflags;
    asm volatile("pushfq; popq %0" : "=r"(rflags));
    return rflags & (1ULL << 9);   /* RFLAGS.IF is bit 9 */
}

/* ================================================================
 * T410 THERMAL SAFETY — self-contained in pit.c
 * ================================================================
 * The ThinkPad T410's Core i5 (Arrandale / Westmere) has a Digital Thermal
 * Sensor (DTS) readable via MSR 0x19C (IA32_THERM_STATUS).  We probe for
 * it at init time (CPUID leaf 6, EAX bit 0) and, once per second inside the
 * timer IRQ, read the thermal margin and log warnings if the CPU is
 * overheating or being hardware-throttled (PROCHOT#).
 *
 * This block is entirely self-contained: it uses only rdmsr() (inline in
 * x86_64.h) and kprintf() -- no dependency on the power subsystem, which is
 * not compiled into the default build.  The full power/thermal.c is upgraded
 * too (real DTS read instead of fake 45 C) but that code is only active when
 * the power subsystem is linked.
 *
 * Fan control is NOT attempted -- the T410 fan is managed by the Embedded
 * Controller (EC register 0x2F via ACPI), which we don't have yet.
 */

#define MSR_IA32_THERM_STATUS       0x19C
#define MSR_IA32_TEMPERATURE_TARGET 0x1A2

#define THERM_STATUS_THROTTLING     (1U << 0)
#define THERM_STATUS_READOUT_MASK   0x007F0000U   /* bits 22:16 */
#define THERM_STATUS_READOUT_SHIFT  16
#define THERM_STATUS_VALID          (1U << 31)

#define THERMAL_WARN_C     85
#define THERMAL_HIGH_C     90
#define THERMAL_CRITICAL_C 95
#define TJMAX_DEFAULT_C    100

static bool     dts_available     = false;
static uint32_t tjmax_c           = TJMAX_DEFAULT_C;
static bool     throttle_warned   = false;
static bool     warm_warned       = false;

/*
 * thermal_probe_dts() — called once from pit_init().
 *
 * Checks CPUID leaf 6 for the DTS feature bit and, on Intel family-6 CPUs,
 * reads TjMax from MSR 0x1A2.  Safe on any x86-64 CPU: if DTS is absent the
 * per-second check becomes a no-op.
 */
static void thermal_probe_dts(void) {
    uint32_t max_leaf;
    asm volatile("cpuid" : "=a"(max_leaf) : "a"(0) : "ebx", "ecx", "edx");
    if (max_leaf < 6)
        return;   /* CPU too old for leaf 6 */

    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(6), "c"(0));

    if (!(eax & 1))
        return;   /* no DTS */

    dts_available = true;

    /* Try to read TjMax from MSR 0x1A2 (bits 23:16) on Intel family 6. */
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(1), "c"(0));
    uint32_t family = (eax >> 8) & 0xF;

    if (family == 6) {
        uint64_t target = rdmsr(MSR_IA32_TEMPERATURE_TARGET);
        uint32_t read_tj = (target >> 16) & 0xFF;
        if (read_tj >= 80 && read_tj <= 120)
            tjmax_c = read_tj;
    }

    kprintf("[THERMAL] DTS active: TjMax=%u C (safety tick enabled)\n", tjmax_c);
}

/*
 * thermal_safety_tick_irq() — called from timer_handler() every 1000 ticks.
 *
 * In IRQ context with interrupts disabled.  Reads a single MSR, does a few
 * integer comparisons, and conditionally logs.  Fast and non-blocking.
 */
static void thermal_safety_tick_irq(void) {
    if (!dts_available)
        return;

    uint64_t raw = rdmsr(MSR_IA32_THERM_STATUS);
    uint32_t status = (uint32_t)raw;

    /* 1. Detect hardware throttling (PROCHOT#) */
    if (status & THERM_STATUS_THROTTLING) {
        if (!throttle_warned) {
            kprintf("[THERMAL] WARNING: CPU is hardware-throttling (PROCHOT#)\n");
            throttle_warned = true;
        }
    } else {
        throttle_warned = false;
    }

    /* 2. Compute temperature from thermal margin */
    uint32_t readout = (status & THERM_STATUS_READOUT_MASK) >> THERM_STATUS_READOUT_SHIFT;
    if (readout == 0 && !(status & THERM_STATUS_VALID))
        return;   /* no valid reading (e.g. QEMU without KVM) */

    uint32_t temp_c = (readout <= tjmax_c) ? (tjmax_c - readout) : 0;

    /* 3. Graded warnings:
     *    >= 95 C : CRITICAL, logged every second
     *    >= 90 C : HIGH, logged every second
     *    >= 85 C : WARM, logged once per crossing
     *    <  85 C : quiet, re-arms the one-shot */
    if (temp_c >= THERMAL_CRITICAL_C) {
        kprintf("[THERMAL] *** CRITICAL: CPU %u C >= %u C! ***\n",
                temp_c, THERMAL_CRITICAL_C);
    } else if (temp_c >= THERMAL_HIGH_C) {
        kprintf("[THERMAL] HIGH: CPU %u C (TjMax=%u, margin=%u)\n",
                temp_c, tjmax_c, tjmax_c - temp_c);
        warm_warned = true;
    } else if (temp_c >= THERMAL_WARN_C) {
        if (!warm_warned) {
            kprintf("[THERMAL] WARM: CPU %u C -- approaching thermal limit\n",
                    temp_c);
            warm_warned = true;
        }
    } else {
        warm_warned = false;
    }
}

// Timer interrupt handler (IRQ0)
// COOPERATIVE MODE: only increments the tick counter.
// Does NOT call schedule() -- preemptive scheduling is deferred.
// EOI is sent by the IRQ dispatch layer (idt.c:irq_handler) after we return.
static void timer_handler(void) {
    timer_ticks++;
    // Real-sleep wakeups: re-ready any process whose blocking-sleep deadline has
    // arrived (1 tick == 1 ms at 1000 Hz). Runs once per tick here (the
    // cooperative IRQ0 path); the PREEMPTIVE build does the equivalent at the top
    // of schedule_from_irq(). Interrupts are disabled in the IRQ handler, so the
    // sleep-list walk is race-free against sys_sleep's push.
    sleep_list_wake_due(timer_ticks);
    // Per-process CPU accounting: charge this 1000 Hz tick to whatever process is
    // currently RUNNING. Counter only -- does not touch total_time or switch logic.
    // (The PREEMPTIVE build does the equivalent once at the top of
    // schedule_from_irq(), which owns IRQ0 instead of this handler.)
    process_t* c = process_get_current();
    if (c) {
        c->cpu_ticks++;
        // STACK CANARY CHECK: verify the running process's kernel stack has not
        // overflowed. Checking every 1000 Hz tick catches overflows faster than
        // waiting for the next context switch.
        STACK_CANARY_CHECK(c);
    }
    scheduler_tick();  // SMP load balancing - redistributes processes across CPUs

    // T410 thermal safety: read the DTS once per second (every 1000 ticks at
    // 1000 Hz).  The function is a fast rdmsr + compare; safe in IRQ context.
    // Self-contained here because the power subsystem is not in the default build.
    if ((timer_ticks % 1000) == 0)
        thermal_safety_tick_irq();

#ifdef SCHED_DEBUG
    // LIVENESS HEARTBEAT (diagnostic): cycle a 48x48 square through red→green→
    // blue→yellow in the top-right corner ~4x/sec. A filled rect fully overwrites
    // its area (no font OR-smear), so it's unambiguous. If this keeps cycling while
    // the rest of the screen is frozen, the KERNEL + TIMER IRQ are ALIVE and the
    // hang is a cooperative ring-3 livelock (init/compositor spinning, which the
    // cooperative scheduler cannot preempt) or a process blocked forever. If the
    // square FREEZES on one colour, the CPU itself is dead (triple fault / IF=0
    // kernel spin / hard hang).
    if ((timer_ticks % 250) == 0) {
        extern void framebuffer_draw_rect(uint32_t, uint32_t, uint32_t,
                                          uint32_t, uint32_t);
        extern void framebuffer_puts_scaled(const char*, uint32_t, uint32_t,
                                            uint32_t, uint32_t);
        static const uint32_t hbcol[4] = {
            0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0x00FFFF00u
        };
        static uint32_t hi = 0;
        framebuffer_draw_rect(1180, 12, 48, 48, hbcol[hi++ & 3]);
        // Show the CURRENTLY-RUNNING process name (clear the cell first to avoid
        // font OR-smear). When frozen, this names WHO is stuck: "init" = init
        // spinning/waiting; "compositor" = compositor spinning; "idle" = every
        // process is blocked (a deadlock — scheduler has nothing to run).
        framebuffer_draw_rect(740, 12, 420, 28, 0x00102040u); /* dark-blue clear */
        if (c)
            framebuffer_puts_scaled(c->name, 744, 14, 0x00FFFFFFu, 2);
    }
#endif
}

// PREEMPTIVE build: irq0_preempt -> schedule_from_irq() OWNS IRQ0 instead of
// timer_handler() above, and calls pit_tick() to advance the millisecond tick
// counter (scheduler.c provides a weak no-op fallback). Without this STRONG
// definition, timer_ticks would never advance in PREEMPT -- freezing
// timer_get_ticks(), blocking sleep wakeups, and uptime. Defined here because
// timer_ticks is file-static to pit.c. Harmless in the cooperative build (where
// pit_tick() is never called -- timer_handler() does the increment instead).
void pit_tick(void) {
    timer_ticks++;
}

// Initialize PIT
void pit_init(uint32_t frequency) {
    kprintf("[PIT] Initializing Programmable Interval Timer...\n");

    if (frequency == 0 || frequency > PIT_BASE_FREQUENCY) {
        kprintf("[PIT] Invalid frequency %u Hz, using 100 Hz\n", frequency);
        frequency = 100;
    }

    /*
     * FIX-PIT-1: Compute divisor with nearest-integer rounding rather than
     * truncation.  For 1000 Hz: 1193182 / 1000 = 1193.182, so the correct
     * divisor is 1193 (since 0.182 < 0.5 -- truncation and rounding agree
     * here, but explicit rounding is correct in general).
     *
     * After clamping, derive timer_frequency from the ACTUAL divisor so that
     * timer_get_ticks_ms() and timer_sleep() use a consistent tick rate.
     */
    uint32_t divisor = (uint32_t)((PIT_BASE_FREQUENCY + frequency / 2) / frequency);

    if (divisor == 0) divisor = 1;          /* guard against /0 */
    if (divisor > 65535) {
        kprintf("[PIT] Warning: divisor %u too large, clamping to 65535\n", divisor);
        divisor = 65535;
    }

    /* Actual frequency the hardware will generate from this divisor. */
    timer_frequency = (uint32_t)(PIT_BASE_FREQUENCY / divisor);

    kprintf("[PIT] Setting frequency to %u Hz (requested %u Hz, divisor: %u)\n",
            timer_frequency, frequency, divisor);

    // Send command byte:
    // Channel 0, Rate Generator (Mode 2), Read/Write LSB then MSB, Binary mode.
    outb(PIT_COMMAND, PIT_CMD_CHANNEL0 | PIT_CMD_RW_BOTH | PIT_CMD_MODE2 | PIT_CMD_BINARY);

    // Send divisor LSB then MSB.
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    // Register IRQ0 handler.
    irq_register_handler(0, timer_handler);

    kprintf("[PIT] Timer initialized at %u Hz\n", timer_frequency);
    kprintf("[PIT] Each tick is approximately %u ms\n",
            timer_frequency ? (1000 / timer_frequency) : 0);

    // T410 thermal safety: probe for the Digital Thermal Sensor so the
    // once-per-second thermal_safety_tick_irq() knows whether to read MSR 0x19C.
    thermal_probe_dts();
}

// Get current tick count
uint64_t timer_get_ticks(void) {
    return timer_ticks;
}

// Get milliseconds since boot: ticks * 1000 / freq
// Uses integer arithmetic; result is monotonically non-decreasing.
uint64_t timer_get_ticks_ms(void) {
    if (timer_frequency == 0) {
        return 0;
    }
    return timer_ticks * 1000ULL / timer_frequency;
}

// Get timer frequency
uint32_t timer_get_frequency(void) {
    return timer_frequency;
}

/*
 * timer_sleep -- busy-wait for at least `ms` milliseconds.
 *
 * FIX-PIT-2: `ms * timer_frequency` was computed as a 32-bit product, which
 * wraps for ms > ~4.29e6 (about 71 minutes at 1000 Hz).  Promote to uint64_t
 * before the multiply so the division result fits without overflow.
 *
 * FIX-PIT-3: hlt suspends the CPU until the next interrupt.  If called with
 * the Interrupt Flag (IF) clear -- e.g. from inside a critical section --
 * hlt never returns and the system hangs.  We detect IF via RFLAGS and fall
 * back to a bare spin if interrupts are disabled, so at worst the caller busy-
 * loops (timer_ticks can't advance when IF=0, but the caller should not be
 * sleeping with interrupts off anyway).
 */
void timer_sleep(uint32_t ms) {
    if (timer_frequency == 0 || ms == 0) return;

    /*
     * FIX-PIT-2: promote ms and timer_frequency to uint64_t so the product
     * never overflows.  At 1000 Hz the old 32-bit code wrapped for ms >= ~71
     * minutes; the new code is safe for any uint32_t ms value.
     */
    uint64_t ticks_to_wait = ((uint64_t)ms * (uint64_t)timer_frequency + 999ULL) / 1000ULL;
    uint64_t target = timer_ticks + ticks_to_wait;

    /*
     * FIX-PIT-3: Only use hlt when IF is set.  With IF=0, hlt stalls forever
     * because IRQ0 can never arrive.  Spin instead (logging a warning once).
     */
    if (pit_rflags_if()) {
        /* Normal path: interrupts enabled, hlt saves power. */
        while (timer_ticks < target) {
            hlt();
        }
    } else {
        /*
         * Interrupts disabled -- we cannot rely on ticks advancing.  The
         * caller is sleeping with IF=0, which is almost certainly a bug;
         * log it once and spin.  We do NOT call sti() here because enabling
         * interrupts inside a caller's critical section would be far worse.
         */
        static int warned = 0;
        if (!warned) {
            kprintf("[PIT] WARNING: timer_sleep() called with interrupts disabled"
                    " -- ticks will not advance!\n");
            warned = 1;
        }
        /* Spin; the loop exits immediately if target already passed. */
        while (timer_ticks < target) {
            asm volatile("pause");   /* reduce power/bus contention */
        }
    }
}
