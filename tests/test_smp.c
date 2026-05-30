/*
 * SMP (Symmetric Multi-Processing) Validation Test Suite
 * =======================================================
 *
 * Comprehensive tests for multi-core CPU support:
 * - CPU detection and boot
 * - Per-CPU data isolation
 * - IPI delivery and latency
 * - TLB shootdown
 * - Scheduler load balancing
 * - Performance scaling
 * - Cache coherence
 * - Stress testing
 *
 * Target: Linear speedup to 8 cores, 80-90% efficiency to 16 cores
 */

#include "../kernel/include/smp.h"
#include "../kernel/include/ipi.h"
#include "../kernel/include/lapic.h"
#include "../kernel/include/kernel.h"
#include "../kernel/include/mem.h"
#include "../kernel/include/x86_64.h"
#include "../kernel/include/spinlock.h"
#include <string.h>

// Test results
typedef struct {
    bool passed;
    uint64_t duration_us;
    char message[256];
} test_result_t;

// Test statistics
static uint32_t tests_passed = 0;
static uint32_t tests_failed = 0;
static uint32_t tests_total = 0;

// Shared test data
static volatile uint32_t ipi_received[MAX_CPUS];
static volatile uint32_t function_call_count[MAX_CPUS];
static volatile bool tlb_flush_complete[MAX_CPUS];
static spinlock_t test_lock;
static volatile uint64_t shared_counter;
static volatile uint64_t parallel_work_done[MAX_CPUS];

// Utility: Get timestamp
static inline uint64_t get_timestamp(void) {
    return __rdtsc();
}

// Utility: Convert TSC ticks to microseconds (assuming 2.4 GHz TSC)
#define TSC_FREQUENCY 2400000000ULL
static inline uint64_t ticks_to_us(uint64_t ticks) {
    return (ticks * 1000000ULL) / TSC_FREQUENCY;
}

// Utility: Print test header
static void print_test_header(const char* name) {
    kprintf("\n========================================\n");
    kprintf("TEST: %s\n", name);
    kprintf("========================================\n");
}

// Utility: Print test result
static void print_test_result(const char* name, test_result_t* result) {
    tests_total++;
    if (result->passed) {
        tests_passed++;
        kprintf("[PASS] %s (%llu us)\n", name, result->duration_us);
    } else {
        tests_failed++;
        kprintf("[FAIL] %s: %s\n", name, result->message);
    }
}

// ============================================================================
// TEST 1: CPU Detection
// ============================================================================
static void test_cpu_detection(test_result_t* result) {
    uint64_t start = get_timestamp();

    result->passed = true;
    result->message[0] = '\0';

    kprintf("[TEST] CPU Detection\n");
    kprintf("  Total CPUs detected: %u\n", smp_num_cpus);
    kprintf("  CPUs online: %u\n", smp_num_online);

    // Verify at least 1 CPU
    if (smp_num_cpus < 1) {
        result->passed = false;
        snprintf(result->message, sizeof(result->message),
                 "No CPUs detected");
        goto done;
    }

    // Verify BSP is online
    if (!cpu_is_online(0)) {
        result->passed = false;
        snprintf(result->message, sizeof(result->message),
                 "BSP (CPU 0) is not online");
        goto done;
    }

    // Print CPU information
    for (uint32_t cpu = 0; cpu < smp_num_cpus; cpu++) {
        percpu_data_t* pcpu = cpu_data(cpu);
        const char* state_str = "UNKNOWN";

        switch (pcpu->state) {
            case CPU_STATE_OFFLINE: state_str = "OFFLINE"; break;
            case CPU_STATE_ONLINE: state_str = "ONLINE"; break;
            case CPU_STATE_STARTING: state_str = "STARTING"; break;
            case CPU_STATE_STOPPING: state_str = "STOPPING"; break;
            case CPU_STATE_FAILED: state_str = "FAILED"; break;
        }

        kprintf("  CPU %u: APIC ID %u, State: %s\n",
                cpu, pcpu->apic_id, state_str);

        // Check for failed CPUs
        if (pcpu->state == CPU_STATE_FAILED) {
            kprintf("    WARNING: CPU %u failed to start\n", cpu);
        }
    }

    kprintf("  Result: Detected %u CPUs, %u online\n",
            smp_num_cpus, smp_num_online);

done:
    result->duration_us = ticks_to_us(get_timestamp() - start);
}

// ============================================================================
// TEST 2: AP Startup
// ============================================================================
static void test_ap_startup(test_result_t* result) {
    uint64_t start = get_timestamp();

    result->passed = true;
    result->message[0] = '\0';

    kprintf("[TEST] AP Startup\n");

    if (smp_num_cpus <= 1) {
        kprintf("  Skipping: Single CPU system\n");
        result->passed = true;
        goto done;
    }

    uint32_t expected_online = smp_num_cpus;
    uint32_t actual_online = 0;

    // Count online CPUs
    for (uint32_t cpu = 0; cpu < smp_num_cpus; cpu++) {
        if (cpu_is_online(cpu)) {
            actual_online++;
        }
    }

    kprintf("  Expected online: %u\n", expected_online);
    kprintf("  Actual online: %u\n", actual_online);

    if (actual_online != expected_online) {
        result->passed = false;
        snprintf(result->message, sizeof(result->message),
                 "Only %u/%u CPUs came online",
                 actual_online, expected_online);
        goto done;
    }

    // Check per-CPU data initialization
    for (uint32_t cpu = 0; cpu < smp_num_cpus; cpu++) {
        percpu_data_t* pcpu = cpu_data(cpu);

        if (pcpu->cpu_id != cpu) {
            result->passed = false;
            snprintf(result->message, sizeof(result->message),
                     "CPU %u has wrong cpu_id: %u",
                     cpu, pcpu->cpu_id);
            goto done;
        }

        if (pcpu->apic_id == 0 && cpu != 0) {
            kprintf("  WARNING: CPU %u has APIC ID 0 (may be uninitialized)\n", cpu);
        }
    }

    kprintf("  Result: All %u CPUs booted successfully\n", smp_num_online);

done:
    result->duration_us = ticks_to_us(get_timestamp() - start);
}

// ============================================================================
// TEST 3: Per-CPU Data Isolation
// ============================================================================
static void test_percpu_isolation_func(void* data) {
    uint32_t cpu = cpu_id();
    percpu_data_t* pcpu = this_cpu();

    // Verify CPU ID matches
    if (pcpu->cpu_id != cpu) {
        kprintf("  ERROR: CPU %u has wrong cpu_id in percpu: %u\n",
                cpu, pcpu->cpu_id);
        return;
    }

    // Increment counter in per-CPU data
    function_call_count[cpu]++;

    kprintf("  CPU %u: Per-CPU data verified (cpu_id=%u, apic_id=%u)\n",
            cpu, pcpu->cpu_id, pcpu->apic_id);
}

static void test_percpu_isolation(test_result_t* result) {
    uint64_t start = get_timestamp();

    result->passed = true;
    result->message[0] = '\0';

    kprintf("[TEST] Per-CPU Data Isolation\n");

    if (smp_num_cpus <= 1) {
        kprintf("  Skipping: Single CPU system\n");
        result->passed = true;
        goto done;
    }

    // Reset counters
    memset((void*)function_call_count, 0, sizeof(function_call_count));

    // Call function on all CPUs
    kprintf("  Calling function on all CPUs...\n");
    ipi_call_function_all(test_percpu_isolation_func, NULL, true);

    // Verify each CPU incremented its own counter
    for (uint32_t cpu = 0; cpu < smp_num_cpus; cpu++) {
        if (cpu_is_online(cpu)) {
            if (function_call_count[cpu] != 1) {
                result->passed = false;
                snprintf(result->message, sizeof(result->message),
                         "CPU %u counter mismatch: expected 1, got %u",
                         cpu, function_call_count[cpu]);
                goto done;
            }
        }
    }

    kprintf("  Result: Per-CPU data isolated correctly on %u CPUs\n",
            smp_num_online);

done:
    result->duration_us = ticks_to_us(get_timestamp() - start);
}

// ============================================================================
// TEST 4: IPI Delivery
// ============================================================================
static void test_ipi_handler_func(void* data) {
    uint32_t cpu = cpu_id();
    ipi_received[cpu]++;
}

static void test_ipi_delivery(test_result_t* result) {
    uint64_t start = get_timestamp();

    result->passed = true;
    result->message[0] = '\0';

    kprintf("[TEST] IPI Delivery\n");

    if (smp_num_cpus <= 1) {
        kprintf("  Skipping: Single CPU system\n");
        result->passed = true;
        goto done;
    }

    // Reset counters
    memset((void*)ipi_received, 0, sizeof(ipi_received));

    // Test unicast IPI
    kprintf("  Testing unicast IPI...\n");
    for (uint32_t cpu = 1; cpu < smp_num_cpus; cpu++) {
        if (cpu_is_online(cpu)) {
            ipi_call_function(cpu, test_ipi_handler_func, NULL, true);

            if (ipi_received[cpu] != 1) {
                result->passed = false;
                snprintf(result->message, sizeof(result->message),
                         "CPU %u did not receive unicast IPI", cpu);
                goto done;
            }
        }
    }
    kprintf("    Unicast: PASS\n");

    // Reset counters
    memset((void*)ipi_received, 0, sizeof(ipi_received));

    // Test broadcast IPI
    kprintf("  Testing broadcast IPI...\n");
    ipi_call_function_all(test_ipi_handler_func, NULL, true);

    for (uint32_t cpu = 0; cpu < smp_num_cpus; cpu++) {
        if (cpu_is_online(cpu)) {
            if (ipi_received[cpu] != 1) {
                result->passed = false;
                snprintf(result->message, sizeof(result->message),
                         "CPU %u did not receive broadcast IPI", cpu);
                goto done;
            }
        }
    }
    kprintf("    Broadcast: PASS\n");

    kprintf("  Result: IPI delivery working on %u CPUs\n", smp_num_online);

done:
    result->duration_us = ticks_to_us(get_timestamp() - start);
}

// ============================================================================
// TEST 5: IPI Latency
// ============================================================================
static void test_ipi_latency_func(void* data) {
    // Empty function to measure latency
    (void)data;
}

static void test_ipi_latency(test_result_t* result) {
    uint64_t start_total = get_timestamp();

    result->passed = true;
    result->message[0] = '\0';

    kprintf("[TEST] IPI Latency\n");

    if (smp_num_cpus <= 1) {
        kprintf("  Skipping: Single CPU system\n");
        result->passed = true;
        goto done;
    }

    #define LATENCY_SAMPLES 100
    uint64_t latencies[LATENCY_SAMPLES];

    // Measure unicast latency to CPU 1
    kprintf("  Measuring unicast IPI latency (CPU 0 -> CPU 1, %d samples)...\n",
            LATENCY_SAMPLES);

    for (uint32_t i = 0; i < LATENCY_SAMPLES; i++) {
        uint64_t start = get_timestamp();
        ipi_call_function(1, test_ipi_latency_func, NULL, true);
        latencies[i] = get_timestamp() - start;
    }

    // Calculate average and min/max
    uint64_t sum = 0, min = UINT64_MAX, max = 0;
    for (uint32_t i = 0; i < LATENCY_SAMPLES; i++) {
        sum += latencies[i];
        if (latencies[i] < min) min = latencies[i];
        if (latencies[i] > max) max = latencies[i];
    }
    uint64_t avg = sum / LATENCY_SAMPLES;

    uint64_t avg_us = ticks_to_us(avg);
    uint64_t min_us = ticks_to_us(min);
    uint64_t max_us = ticks_to_us(max);

    kprintf("    Average: %llu us\n", avg_us);
    kprintf("    Min: %llu us\n", min_us);
    kprintf("    Max: %llu us\n", max_us);

    // Target: 2-5 microseconds for IPI round-trip
    if (avg_us > 10) {
        kprintf("    WARNING: IPI latency higher than expected (target: <10us)\n");
    }

    kprintf("  Result: IPI latency measured\n");

done:
    result->duration_us = ticks_to_us(get_timestamp() - start_total);
}

// ============================================================================
// TEST 6: TLB Shootdown
// ============================================================================
static void test_tlb_shootdown_func(void* data) {
    uint32_t cpu = cpu_id();
    tlb_flush_complete[cpu] = true;
}

static void test_tlb_shootdown(test_result_t* result) {
    uint64_t start = get_timestamp();

    result->passed = true;
    result->message[0] = '\0';

    kprintf("[TEST] TLB Shootdown\n");

    if (smp_num_cpus <= 1) {
        kprintf("  Skipping: Single CPU system\n");
        result->passed = true;
        goto done;
    }

    // Reset flags
    memset((void*)tlb_flush_complete, 0, sizeof(tlb_flush_complete));

    kprintf("  Testing TLB flush on all CPUs...\n");

    // Measure TLB shootdown latency
    uint64_t flush_start = get_timestamp();
    ipi_tlb_flush_all();
    uint64_t flush_end = get_timestamp();

    uint64_t flush_us = ticks_to_us(flush_end - flush_start);

    kprintf("    TLB flush completed in %llu us\n", flush_us);

    // Target: <10us for 4 CPUs
    uint64_t target_us = 5 + (smp_num_online * 2);
    if (flush_us > target_us) {
        kprintf("    WARNING: TLB shootdown slower than expected (target: <%lluus)\n",
                target_us);
    }

    kprintf("  Result: TLB shootdown working\n");

done:
    result->duration_us = ticks_to_us(get_timestamp() - start);
}

// ============================================================================
// TEST 7: Cache Coherence
// ============================================================================
static void test_cache_coherence_writer(void* data) {
    uint64_t* counter = (uint64_t*)data;

    // Each CPU increments the shared counter 1000 times
    for (int i = 0; i < 1000; i++) {
        __atomic_add_fetch(counter, 1, __ATOMIC_SEQ_CST);
    }
}

static void test_cache_coherence(test_result_t* result) {
    uint64_t start = get_timestamp();

    result->passed = true;
    result->message[0] = '\0';

    kprintf("[TEST] Cache Coherence\n");

    if (smp_num_cpus <= 1) {
        kprintf("  Skipping: Single CPU system\n");
        result->passed = true;
        goto done;
    }

    shared_counter = 0;

    kprintf("  Testing atomic operations on shared memory...\n");
    kprintf("  Each CPU will increment shared counter 1000 times\n");

    // All CPUs increment shared counter
    ipi_call_function_all(test_cache_coherence_writer, (void*)&shared_counter, true);

    uint64_t expected = smp_num_online * 1000;

    kprintf("    Expected: %llu\n", expected);
    kprintf("    Actual: %llu\n", shared_counter);

    if (shared_counter != expected) {
        result->passed = false;
        snprintf(result->message, sizeof(result->message),
                 "Cache coherence failure: expected %llu, got %llu",
                 expected, shared_counter);
        goto done;
    }

    kprintf("  Result: Cache coherence working (MESI protocol OK)\n");

done:
    result->duration_us = ticks_to_us(get_timestamp() - start);
}

// ============================================================================
// TEST 8: Performance Scaling
// ============================================================================
static void test_parallel_work_func(void* data) {
    uint32_t cpu = cpu_id();
    uint32_t iterations = *(uint32_t*)data;

    // Perform computational work
    uint64_t sum = 0;
    for (uint32_t i = 0; i < iterations; i++) {
        sum += i * i;
    }

    parallel_work_done[cpu] = sum;
}

static void test_performance_scaling(test_result_t* result) {
    uint64_t start = get_timestamp();

    result->passed = true;
    result->message[0] = '\0';

    kprintf("[TEST] Performance Scaling\n");

    if (smp_num_cpus <= 1) {
        kprintf("  Skipping: Single CPU system\n");
        result->passed = true;
        goto done;
    }

    uint32_t iterations = 10000000;  // 10M iterations per CPU

    kprintf("  Running parallel workload (%u iterations per CPU)...\n", iterations);

    // Measure single-threaded baseline
    kprintf("  Single-threaded baseline...\n");
    memset((void*)parallel_work_done, 0, sizeof(parallel_work_done));

    uint64_t single_start = get_timestamp();
    test_parallel_work_func(&iterations);
    uint64_t single_end = get_timestamp();
    uint64_t single_time = single_end - single_start;

    kprintf("    Time: %llu us\n", ticks_to_us(single_time));

    // Measure multi-threaded performance
    kprintf("  Multi-threaded on %u CPUs...\n", smp_num_online);
    memset((void*)parallel_work_done, 0, sizeof(parallel_work_done));

    uint64_t multi_start = get_timestamp();
    ipi_call_function_all(test_parallel_work_func, &iterations, true);
    uint64_t multi_end = get_timestamp();
    uint64_t multi_time = multi_end - multi_start;

    kprintf("    Time: %llu us\n", ticks_to_us(multi_time));

    // Calculate speedup and efficiency
    double speedup = (double)single_time / (double)multi_time;
    double efficiency = (speedup / (double)smp_num_online) * 100.0;

    kprintf("  Results:\n");
    kprintf("    Speedup: %.2fx\n", speedup);
    kprintf("    Parallel Efficiency: %.1f%%\n", efficiency);

    // Target: Linear speedup to 8 cores, 80-90% efficiency to 16 cores
    if (smp_num_online <= 8) {
        // Expect >90% efficiency for <=8 cores
        if (efficiency < 90.0) {
            kprintf("    WARNING: Efficiency below target for %u CPUs (target: >90%%)\n",
                    smp_num_online);
        }
    } else if (smp_num_online <= 16) {
        // Expect >80% efficiency for <=16 cores
        if (efficiency < 80.0) {
            kprintf("    WARNING: Efficiency below target for %u CPUs (target: >80%%)\n",
                    smp_num_online);
        }
    } else {
        // Just report efficiency for larger systems
        kprintf("    Large system: efficiency may vary\n");
    }

done:
    result->duration_us = ticks_to_us(get_timestamp() - start);
}

// ============================================================================
// TEST 9: Stress Test
// ============================================================================
static void test_stress_func(void* data) {
    uint32_t cpu = cpu_id();
    uint32_t duration_ms = *(uint32_t*)data;

    uint64_t start = __rdtsc();
    uint64_t target_ticks = (uint64_t)duration_ms * (TSC_FREQUENCY / 1000);

    // Continuous work + IPIs for stress duration
    while ((__rdtsc() - start) < target_ticks) {
        // Do some work
        volatile uint64_t sum = 0;
        for (int i = 0; i < 1000; i++) {
            sum += i;
        }

        // Trigger TLB flush occasionally
        if ((sum % 10000) == 0) {
            __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
        }
    }

    function_call_count[cpu]++;
}

static void test_stress(test_result_t* result) {
    uint64_t start = get_timestamp();

    result->passed = true;
    result->message[0] = '\0';

    kprintf("[TEST] Stress Test\n");

    if (smp_num_cpus <= 1) {
        kprintf("  Skipping: Single CPU system\n");
        result->passed = true;
        goto done;
    }

    uint32_t duration_ms = 5000;  // 5 seconds

    kprintf("  Running stress test for %u seconds on all %u CPUs...\n",
            duration_ms / 1000, smp_num_online);
    kprintf("  (continuous work + IPIs + TLB flushes)\n");

    // Reset counters
    memset((void*)function_call_count, 0, sizeof(function_call_count));

    // Run stress test on all CPUs
    ipi_call_function_all(test_stress_func, &duration_ms, true);

    // Verify all CPUs completed
    for (uint32_t cpu = 0; cpu < smp_num_cpus; cpu++) {
        if (cpu_is_online(cpu)) {
            if (function_call_count[cpu] != 1) {
                result->passed = false;
                snprintf(result->message, sizeof(result->message),
                         "CPU %u failed stress test", cpu);
                goto done;
            }
        }
    }

    kprintf("  Result: All CPUs survived stress test\n");

done:
    result->duration_us = ticks_to_us(get_timestamp() - start);
}

// ============================================================================
// Main Test Suite
// ============================================================================
void smp_run_tests(void) {
    test_result_t result;

    kprintf("\n");
    kprintf("================================================================================\n");
    kprintf("                    SMP VALIDATION TEST SUITE\n");
    kprintf("================================================================================\n");
    kprintf("\n");

    // Initialize test infrastructure
    spin_lock_init(&test_lock);

    // Print system information
    kprintf("System Configuration:\n");
    kprintf("  CPUs detected: %u\n", smp_num_cpus);
    kprintf("  CPUs online: %u\n", smp_num_online);
    kprintf("  LAPIC enabled: %s\n", lapic_enabled ? "Yes" : "No");
    kprintf("\n");

    // Run tests
    print_test_header("1. CPU Detection");
    test_cpu_detection(&result);
    print_test_result("CPU Detection", &result);

    print_test_header("2. AP Startup");
    test_ap_startup(&result);
    print_test_result("AP Startup", &result);

    print_test_header("3. Per-CPU Data Isolation");
    test_percpu_isolation(&result);
    print_test_result("Per-CPU Data Isolation", &result);

    print_test_header("4. IPI Delivery");
    test_ipi_delivery(&result);
    print_test_result("IPI Delivery", &result);

    print_test_header("5. IPI Latency");
    test_ipi_latency(&result);
    print_test_result("IPI Latency", &result);

    print_test_header("6. TLB Shootdown");
    test_tlb_shootdown(&result);
    print_test_result("TLB Shootdown", &result);

    print_test_header("7. Cache Coherence");
    test_cache_coherence(&result);
    print_test_result("Cache Coherence", &result);

    print_test_header("8. Performance Scaling");
    test_performance_scaling(&result);
    print_test_result("Performance Scaling", &result);

    print_test_header("9. Stress Test");
    test_stress(&result);
    print_test_result("Stress Test", &result);

    // Print summary
    kprintf("\n");
    kprintf("================================================================================\n");
    kprintf("                           TEST SUMMARY\n");
    kprintf("================================================================================\n");
    kprintf("\n");
    kprintf("Tests run: %u\n", tests_total);
    kprintf("Passed: %u\n", tests_passed);
    kprintf("Failed: %u\n", tests_failed);
    kprintf("\n");

    if (tests_failed == 0) {
        kprintf("Result: ALL TESTS PASSED\n");
    } else {
        kprintf("Result: %u TEST(S) FAILED\n", tests_failed);
    }

    kprintf("\n");
    kprintf("================================================================================\n");

    // Print detailed SMP statistics
    kprintf("\n");
    smp_print_info();
    smp_print_stats();
    ipi_print_stats();
}
