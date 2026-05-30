#ifndef BOOT_PROFILE_H
#define BOOT_PROFILE_H

/**
 * Boot Time Profiling Infrastructure
 *
 * Provides fine-grained boot time measurement for each subsystem.
 * Uses RDTSC (Read Time-Stamp Counter) for cycle-accurate timing.
 *
 * Agent 20: Boot Optimization Specialist
 *
 * Usage:
 *   BOOT_PROFILE_INIT();           // At start of kernel_main()
 *   BOOT_PROFILE_START("GDT");     // Before subsystem init
 *   gdt_init();
 *   BOOT_PROFILE_END("GDT");       // After subsystem init
 *   ...
 *   BOOT_PROFILE_REPORT();         // At end of boot
 *
 * Output:
 *   [BOOT PROFILE]
 *   GDT Init:        23.45 ms
 *   IDT Init:        31.20 ms
 *   PMM Init:       187.34 ms
 *   ...
 *   TOTAL:         7729.65 ms
 */

#include "types.h"
#include "kernel.h"
#include "perf.h"

// Maximum number of boot stages to track
#define BOOT_PROFILE_MAX_STAGES 64

// Boot stage entry
typedef struct {
    const char* name;
    uint64_t start_cycles;
    uint64_t end_cycles;
    uint64_t duration_ms;
    bool completed;
} boot_stage_t;

// Boot profile state
typedef struct {
    boot_stage_t stages[BOOT_PROFILE_MAX_STAGES];
    uint32_t stage_count;
    uint64_t boot_start_cycles;
    uint64_t boot_end_cycles;
    bool enabled;
} boot_profile_t;

// Global boot profiler
static boot_profile_t boot_profiler = {0};

/**
 * Initialize boot profiler
 * Call at the very start of kernel_main()
 */
static inline void boot_profile_init(void) {
    boot_profiler.stage_count = 0;
    boot_profiler.boot_start_cycles = rdtsc();
    boot_profiler.enabled = true;
}

/**
 * Start profiling a boot stage
 */
static inline void boot_profile_start(const char* name) {
    if (!boot_profiler.enabled) return;
    if (boot_profiler.stage_count >= BOOT_PROFILE_MAX_STAGES) return;

    boot_stage_t* stage = &boot_profiler.stages[boot_profiler.stage_count];
    stage->name = name;
    stage->start_cycles = rdtsc();
    stage->completed = false;
}

/**
 * End profiling a boot stage
 */
static inline void boot_profile_end(const char* name) {
    if (!boot_profiler.enabled) return;
    if (boot_profiler.stage_count >= BOOT_PROFILE_MAX_STAGES) return;

    boot_stage_t* stage = &boot_profiler.stages[boot_profiler.stage_count];

    // Verify name matches (sanity check)
    if (stage->name != name) {
        kprintf("[BOOT PROFILE] WARNING: Stage name mismatch: expected '%s', got '%s'\n",
                stage->name, name);
        return;
    }

    stage->end_cycles = rdtsc();
    uint64_t duration_cycles = stage->end_cycles - stage->start_cycles;
    stage->duration_ms = cycles_to_ms(duration_cycles);
    stage->completed = true;

    boot_profiler.stage_count++;
}

/**
 * Generate boot profile report
 * Call at the end of kernel_main()
 */
static inline void boot_profile_report(void) {
    if (!boot_profiler.enabled) return;

    boot_profiler.boot_end_cycles = rdtsc();
    uint64_t total_cycles = boot_profiler.boot_end_cycles - boot_profiler.boot_start_cycles;
    uint64_t total_ms = cycles_to_ms(total_cycles);

    kprintf("\n");
    kprintf("=====================================\n");
    kprintf("   BOOT TIME PROFILE REPORT\n");
    kprintf("=====================================\n");
    kprintf("\n");

    // Print each stage
    uint64_t accounted_ms = 0;
    for (uint32_t i = 0; i < boot_profiler.stage_count; i++) {
        boot_stage_t* stage = &boot_profiler.stages[i];

        if (!stage->completed) {
            kprintf("%-24s: INCOMPLETE\n", stage->name);
            continue;
        }

        // Calculate percentage
        uint64_t percent = (stage->duration_ms * 100) / total_ms;
        uint64_t percent_frac = ((stage->duration_ms * 10000) / total_ms) % 100;

        // Format with bar graph
        char bar[32] = {0};
        uint32_t bar_len = (stage->duration_ms * 30) / total_ms;
        if (bar_len > 30) bar_len = 30;

        for (uint32_t j = 0; j < bar_len; j++) {
            bar[j] = '#';
        }

        kprintf("%-24s: %6lu.%02lu ms  [%3lu.%02lu%%] %s\n",
                stage->name,
                (unsigned long)stage->duration_ms,
                (unsigned long)cycles_to_ms_frac(stage->end_cycles - stage->start_cycles),
                (unsigned long)percent,
                (unsigned long)percent_frac,
                bar);

        accounted_ms += stage->duration_ms;
    }

    // Print unaccounted time (overhead)
    uint64_t unaccounted_ms = total_ms - accounted_ms;
    kprintf("\n");
    kprintf("%-24s: %6lu.%02lu ms\n", "Unaccounted (overhead)",
            (unsigned long)unaccounted_ms,
            (unsigned long)((unaccounted_ms * 100) % 100));

    // Print total
    kprintf("%-24s: %6lu.%02lu ms\n", "TOTAL BOOT TIME",
            (unsigned long)total_ms,
            (unsigned long)cycles_to_ms_frac(total_cycles));

    kprintf("\n");
    kprintf("=====================================\n");

    // Identify bottlenecks (stages > 500ms)
    kprintf("\n");
    kprintf("BOTTLENECKS (> 500ms):\n");
    bool found_bottleneck = false;
    for (uint32_t i = 0; i < boot_profiler.stage_count; i++) {
        boot_stage_t* stage = &boot_profiler.stages[i];
        if (stage->completed && stage->duration_ms > 500) {
            kprintf("  - %-24s: %lu.%02lu ms\n",
                    stage->name,
                    (unsigned long)stage->duration_ms,
                    (unsigned long)cycles_to_ms_frac(stage->end_cycles - stage->start_cycles));
            found_bottleneck = true;
        }
    }
    if (!found_bottleneck) {
        kprintf("  (none - all stages < 500ms)\n");
    }

    kprintf("\n");

    // Performance assessment
    kprintf("PERFORMANCE ASSESSMENT:\n");
    if (total_ms < 1500) {
        kprintf("  ✓ EXCELLENT: Boot time < 1.5s (target met)\n");
    } else if (total_ms < 3000) {
        kprintf("  ✓ GOOD: Boot time < 3s (target met)\n");
    } else if (total_ms < 5000) {
        kprintf("  ⚠ ACCEPTABLE: Boot time < 5s (needs optimization)\n");
    } else {
        kprintf("  ✗ SLOW: Boot time > 5s (OPTIMIZATION REQUIRED)\n");
    }

    kprintf("\n");
}

/**
 * Disable boot profiling (to reduce overhead after boot)
 */
static inline void boot_profile_disable(void) {
    boot_profiler.enabled = false;
}

// Convenience macros
#define BOOT_PROFILE_INIT() boot_profile_init()
#define BOOT_PROFILE_START(name) boot_profile_start(name)
#define BOOT_PROFILE_END(name) boot_profile_end(name)
#define BOOT_PROFILE_REPORT() boot_profile_report()
#define BOOT_PROFILE_DISABLE() boot_profile_disable()

/**
 * Scoped boot profiler (RAII-style)
 * Usage:
 *   BOOT_PROFILE_SCOPE("GDT Init");
 *   gdt_init();
 *   // Automatically ends when scope exits
 *
 * Note: Requires compiler support for cleanup attribute
 */
#ifdef __GNUC__
typedef struct {
    const char* name;
} boot_profile_scope_t;

static inline void boot_profile_scope_cleanup(boot_profile_scope_t* scope) {
    boot_profile_end(scope->name);
}

#define BOOT_PROFILE_SCOPE(name) \
    boot_profile_start(name); \
    boot_profile_scope_t __attribute__((cleanup(boot_profile_scope_cleanup))) \
        __boot_scope_##__LINE__ = { .name = name }
#else
// Fallback for non-GCC compilers
#define BOOT_PROFILE_SCOPE(name) \
    boot_profile_start(name)
#endif

/**
 * Export boot profile data (for tooling/analysis)
 * Outputs JSON format for parsing by external tools
 */
static inline void boot_profile_export_json(void) {
    if (!boot_profiler.enabled) return;

    uint64_t total_cycles = boot_profiler.boot_end_cycles - boot_profiler.boot_start_cycles;
    uint64_t total_ms = cycles_to_ms(total_cycles);

    kprintf("{\n");
    kprintf("  \"boot_time_ms\": %lu,\n", (unsigned long)total_ms);
    kprintf("  \"cpu_freq_mhz\": %u,\n", CPU_FREQ_MHZ);
    kprintf("  \"stages\": [\n");

    for (uint32_t i = 0; i < boot_profiler.stage_count; i++) {
        boot_stage_t* stage = &boot_profiler.stages[i];

        kprintf("    {\n");
        kprintf("      \"name\": \"%s\",\n", stage->name);
        kprintf("      \"duration_ms\": %lu,\n", (unsigned long)stage->duration_ms);
        kprintf("      \"duration_cycles\": %lu,\n",
                (unsigned long)(stage->end_cycles - stage->start_cycles));
        kprintf("      \"completed\": %s\n", stage->completed ? "true" : "false");
        kprintf("    }%s\n", (i < boot_profiler.stage_count - 1) ? "," : "");
    }

    kprintf("  ]\n");
    kprintf("}\n");
}

#define BOOT_PROFILE_EXPORT_JSON() boot_profile_export_json()

#endif // BOOT_PROFILE_H
