/*
 * CPU Frequency Scaling (CPUFreq)
 * Dynamic frequency and voltage scaling for power management
 */

#include "../include/power.h"
#include "../include/kernel.h"
#include "../include/io.h"
#include "../include/string.h"

extern power_global_state_t power_global;

// Available CPU frequencies (MHz)
static uint32_t available_frequencies[] = {
    800,    // Lowest
    1200,
    1600,
    2000,
    2400,
    2800,
    3200,
    3600,   // Highest
};

#define NUM_FREQUENCIES (sizeof(available_frequencies) / sizeof(uint32_t))

// MSR (Model Specific Register) for CPU frequency control
#define MSR_IA32_PERF_CTL       0x199
#define MSR_IA32_PERF_STATUS    0x198

/**
 * Read MSR (Model Specific Register)
 */
static uint64_t read_msr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

/**
 * Write MSR
 */
static void write_msr(uint32_t msr, uint64_t value) {
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" :: "a"(low), "d"(high), "c"(msr));
}

/**
 * Set CPU frequency via MSR
 */
static int set_cpu_frequency_hw(uint32_t cpu, uint32_t freq_mhz) {
    // Calculate P-state value (simplified)
    // Real implementation would use ACPI _PSS table
    uint32_t pstate = (freq_mhz / 100) - 8;

    // Write to IA32_PERF_CTL MSR
    write_msr(MSR_IA32_PERF_CTL, pstate << 8);

    return 0;
}

/**
 * Get current CPU frequency from hardware
 */
static uint32_t get_cpu_frequency_hw(uint32_t cpu) {
    // Read from IA32_PERF_STATUS MSR
    uint64_t status = read_msr(MSR_IA32_PERF_STATUS);
    uint32_t pstate = (status >> 8) & 0xFF;

    // Convert P-state to MHz (simplified)
    return (pstate + 8) * 100;
}

/**
 * Apply governor policy
 */
static void cpufreq_apply_governor(uint32_t cpu) {
    cpufreq_policy_t* policy = &power_global.cpu_policies[cpu];

    switch (policy->governor) {
        case CPUFREQ_GOVERNOR_PERFORMANCE:
            // Always max frequency
            cpufreq_set_frequency(cpu, policy->max_freq_mhz);
            break;

        case CPUFREQ_GOVERNOR_POWERSAVE:
            // Always min frequency
            cpufreq_set_frequency(cpu, policy->min_freq_mhz);
            break;

        case CPUFREQ_GOVERNOR_ONDEMAND:
            // TODO: Scale based on CPU load
            // For now, use a middle frequency
            cpufreq_set_frequency(cpu, (policy->min_freq_mhz + policy->max_freq_mhz) / 2);
            break;

        case CPUFREQ_GOVERNOR_CONSERVATIVE:
            // TODO: Like ondemand but with slower changes
            cpufreq_set_frequency(cpu, (policy->min_freq_mhz + policy->max_freq_mhz) / 2);
            break;

        case CPUFREQ_GOVERNOR_USERSPACE:
            // User controls frequency, do nothing
            break;
    }
}

/**
 * Initialize CPU frequency scaling
 */
int cpufreq_init(void) {
    kprintf("[CPUFREQ] Initializing CPU frequency scaling...\n");

    // Get number of CPUs from ACPI
    power_global.num_cpus = acpi_state.num_cpus;
    if (power_global.num_cpus == 0) {
        power_global.num_cpus = 1;  // Assume at least 1 CPU
    }

    // Allocate CPU policies
    power_global.cpu_policies = kmalloc(sizeof(cpufreq_policy_t) * power_global.num_cpus);
    if (!power_global.cpu_policies) {
        kprintf("[CPUFREQ] ERROR: Failed to allocate memory\n");
        return -1;
    }

    // Initialize policies for each CPU
    for (uint32_t i = 0; i < power_global.num_cpus; i++) {
        cpufreq_policy_t* policy = &power_global.cpu_policies[i];

        policy->cpu = i;
        policy->min_freq_mhz = available_frequencies[0];
        policy->max_freq_mhz = available_frequencies[NUM_FREQUENCIES - 1];
        policy->current_freq_mhz = get_cpu_frequency_hw(i);
        policy->governor = CPUFREQ_GOVERNOR_ONDEMAND;

        // Copy available frequencies
        policy->num_freqs = NUM_FREQUENCIES;
        policy->available_freqs = kmalloc(sizeof(uint32_t) * NUM_FREQUENCIES);
        if (policy->available_freqs) {
            memcpy(policy->available_freqs, available_frequencies,
                   sizeof(uint32_t) * NUM_FREQUENCIES);
        }

        policy->transition_latency_ns = 10000;  // 10μs

        kprintf("[CPUFREQ] CPU %u: %u MHz (min %u, max %u)\n",
                i, policy->current_freq_mhz,
                policy->min_freq_mhz, policy->max_freq_mhz);
    }

    return 0;
}

/**
 * Set CPU frequency
 */
int cpufreq_set_frequency(uint32_t cpu, uint32_t freq_mhz) {
    if (cpu >= power_global.num_cpus) {
        return -1;
    }

    cpufreq_policy_t* policy = &power_global.cpu_policies[cpu];

    // Clamp to min/max
    if (freq_mhz < policy->min_freq_mhz) {
        freq_mhz = policy->min_freq_mhz;
    }
    if (freq_mhz > policy->max_freq_mhz) {
        freq_mhz = policy->max_freq_mhz;
    }

    // Find closest available frequency
    uint32_t closest = policy->min_freq_mhz;
    uint32_t min_diff = (uint32_t)-1;

    for (uint32_t i = 0; i < policy->num_freqs; i++) {
        uint32_t diff = (freq_mhz > policy->available_freqs[i]) ?
                        (freq_mhz - policy->available_freqs[i]) :
                        (policy->available_freqs[i] - freq_mhz);

        if (diff < min_diff) {
            min_diff = diff;
            closest = policy->available_freqs[i];
        }
    }

    // Set frequency in hardware
    if (set_cpu_frequency_hw(cpu, closest) < 0) {
        return -1;
    }

    policy->current_freq_mhz = closest;
    return 0;
}

/**
 * Get current CPU frequency
 */
uint32_t cpufreq_get_frequency(uint32_t cpu) {
    if (cpu >= power_global.num_cpus) {
        return 0;
    }

    // Read from hardware
    power_global.cpu_policies[cpu].current_freq_mhz = get_cpu_frequency_hw(cpu);
    return power_global.cpu_policies[cpu].current_freq_mhz;
}

/**
 * Set CPU frequency governor
 */
int cpufreq_set_governor(uint32_t cpu, cpufreq_governor_t governor) {
    if (cpu >= power_global.num_cpus) {
        return -1;
    }

    cpufreq_policy_t* policy = &power_global.cpu_policies[cpu];
    policy->governor = governor;

    kprintf("[CPUFREQ] CPU %u: Set governor to %s\n", cpu,
            governor == CPUFREQ_GOVERNOR_PERFORMANCE ? "performance" :
            governor == CPUFREQ_GOVERNOR_POWERSAVE ? "powersave" :
            governor == CPUFREQ_GOVERNOR_ONDEMAND ? "ondemand" :
            governor == CPUFREQ_GOVERNOR_CONSERVATIVE ? "conservative" :
            "userspace");

    // Apply governor immediately
    cpufreq_apply_governor(cpu);

    return 0;
}

/**
 * Get CPU frequency governor
 */
cpufreq_governor_t cpufreq_get_governor(uint32_t cpu) {
    if (cpu >= power_global.num_cpus) {
        return CPUFREQ_GOVERNOR_ONDEMAND;
    }

    return power_global.cpu_policies[cpu].governor;
}

/**
 * Get CPU frequency policy
 */
cpufreq_policy_t* cpufreq_get_policy(uint32_t cpu) {
    if (cpu >= power_global.num_cpus) {
        return NULL;
    }

    return &power_global.cpu_policies[cpu];
}

/**
 * Set CPU frequency policy
 */
int cpufreq_set_policy(uint32_t cpu, cpufreq_policy_t* policy) {
    if (cpu >= power_global.num_cpus || !policy) {
        return -1;
    }

    memcpy(&power_global.cpu_policies[cpu], policy, sizeof(cpufreq_policy_t));
    cpufreq_apply_governor(cpu);

    return 0;
}
