/**
 * Kernel Time Interface
 *
 * Provides timing functions for the kernel.
 * Uses PIT (Programmable Interval Timer) as the time source.
 */

#ifndef KERNEL_TIME_H
#define KERNEL_TIME_H

#include "types.h"
#include "drivers.h"

/* Timer frequency (ticks per second) - matches PIT configuration */
#define TIMER_FREQ 100

/* Get current timer tick count (from PIT driver) */
static inline uint64_t get_ticks(void) {
    return timer_get_ticks();
}

/* Convert milliseconds to ticks */
static inline uint64_t ms_to_ticks(uint64_t ms) {
    return (ms * TIMER_FREQ) / 1000;
}

/* Convert ticks to milliseconds */
static inline uint64_t ticks_to_ms(uint64_t ticks) {
    return (ticks * 1000) / TIMER_FREQ;
}

#endif /* KERNEL_TIME_H */
