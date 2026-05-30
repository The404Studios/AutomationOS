/*
 * CPU Idle States (C-States)
 * Power management for idle CPUs
 */

#include "../include/power.h"
#include "../include/kernel.h"

extern power_global_state_t power_global;

/**
 * Initialize CPU idle management
 */
int cpuidle_init(void) {
    kprintf("[CPUIDLE] Initializing CPU idle management...\n");

    // Allocate idle state structures
    power_global.cpu_idle_states = kmalloc(sizeof(cpuidle_state_t) * power_global.num_cpus);
    if (!power_global.cpu_idle_states) {
        return -1;
    }

    // Initialize idle states for each CPU
    for (uint32_t i = 0; i < power_global.num_cpus; i++) {
        cpuidle_state_t* state = &power_global.cpu_idle_states[i];

        state->cpu = i;
        state->current_state = ACPI_CSTATE_C0;

        // Default latencies (microseconds)
        state->c1_latency = 1;      // 1μs
        state->c2_latency = 100;    // 100μs
        state->c3_latency = 1000;   // 1ms

        state->c0_time_ms = 0;
        state->c1_time_ms = 0;
        state->c2_time_ms = 0;
        state->c3_time_ms = 0;
    }

    kprintf("[CPUIDLE] Initialized for %u CPUs\n", power_global.num_cpus);
    return 0;
}

/**
 * Enter C-state
 */
int cpuidle_enter_state(uint32_t cpu, acpi_cstate_t state) {
    if (cpu >= power_global.num_cpus) {
        return -1;
    }

    cpuidle_state_t* idle_state = &power_global.cpu_idle_states[cpu];
    idle_state->current_state = state;

    switch (state) {
        case ACPI_CSTATE_C1:
            // Halt instruction
            __asm__ volatile("hlt");
            break;

        case ACPI_CSTATE_C2:
            // Stop-Clock (would need chipset support)
            __asm__ volatile("hlt");
            break;

        case ACPI_CSTATE_C3:
            // Deep sleep (would need ACPI support)
            __asm__ volatile("hlt");
            break;

        default:
            break;
    }

    return 0;
}

/**
 * Get idle state info
 */
cpuidle_state_t* cpuidle_get_state(uint32_t cpu) {
    if (cpu >= power_global.num_cpus) {
        return NULL;
    }
    return &power_global.cpu_idle_states[cpu];
}

/**
 * Get time spent in C-state
 */
uint64_t cpuidle_get_time_in_state(uint32_t cpu, acpi_cstate_t state) {
    cpuidle_state_t* idle_state = cpuidle_get_state(cpu);
    if (!idle_state) {
        return 0;
    }

    switch (state) {
        case ACPI_CSTATE_C0: return idle_state->c0_time_ms;
        case ACPI_CSTATE_C1: return idle_state->c1_time_ms;
        case ACPI_CSTATE_C2: return idle_state->c2_time_ms;
        case ACPI_CSTATE_C3: return idle_state->c3_time_ms;
        default: return 0;
    }
}
