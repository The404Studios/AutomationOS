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

// Timer interrupt handler (IRQ0)
// COOPERATIVE MODE: only increments the tick counter.
// Does NOT call schedule() -- preemptive scheduling is deferred.
// EOI is sent by the IRQ dispatch layer (idt.c:irq_handler) after we return.
static void timer_handler(void) {
    timer_ticks++;
    // Per-process CPU accounting: charge this 1000 Hz tick to whatever process is
    // currently RUNNING. Counter only -- does not touch total_time or switch logic.
    // (The PREEMPTIVE build does the equivalent once at the top of
    // schedule_from_irq(), which owns IRQ0 instead of this handler.)
    process_t* c = process_get_current();
    if (c) {
        c->cpu_ticks++;
    }
    scheduler_tick();  // SMP load balancing - redistributes processes across CPUs
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
