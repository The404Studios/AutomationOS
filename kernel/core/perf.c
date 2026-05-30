#include "../include/perf.h"
#include "../include/kernel.h"
#include "../include/drivers.h"

// Performance monitoring state
static bool perf_enabled = true;
static uint64_t cpu_freq_mhz = CPU_FREQ_MHZ;

/**
 * Calibrate CPU frequency using PIT
 *
 * Uses the PIT (Programmable Interval Timer) as a reference
 * to measure the actual CPU frequency.
 */
void perf_calibrate_cpu_freq(void) {
    kprintf("[PERF] Calibrating CPU frequency...\n");

    // Wait for PIT to stabilize
    timer_sleep(10);

    // Measure TSC ticks over 100ms
    uint64_t ticks_start = timer_get_ticks();
    uint64_t tsc_start = rdtsc();

    timer_sleep(100);  // 100ms

    uint64_t tsc_end = rdtsc();
    uint64_t ticks_end = timer_get_ticks();

    // Calculate frequency
    uint64_t tsc_delta = tsc_end - tsc_start;
    uint64_t ticks_delta = ticks_end - ticks_start;
    uint32_t timer_freq = timer_get_frequency();

    // tsc_delta cycles in ticks_delta timer ticks
    // Frequency = (tsc_delta / ticks_delta) * timer_freq
    uint64_t freq_hz = (tsc_delta * timer_freq) / ticks_delta;
    cpu_freq_mhz = freq_hz / 1000000;

    kprintf("[PERF] CPU frequency: %llu MHz\n", cpu_freq_mhz);
    kprintf("[PERF] TSC delta: %llu cycles in %llu ticks\n", tsc_delta, ticks_delta);
}

void perf_enable(void) {
    perf_enabled = true;
    kprintf("[PERF] Performance monitoring enabled\n");
}

void perf_disable(void) {
    perf_enabled = false;
    kprintf("[PERF] Performance monitoring disabled\n");
}

bool perf_is_enabled(void) {
    return perf_enabled;
}

/**
 * Get calibrated CPU frequency
 */
uint64_t perf_get_cpu_freq_mhz(void) {
    return cpu_freq_mhz;
}
