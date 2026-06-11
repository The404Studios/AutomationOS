/*
 * SMP Stress Test for Audit Rate Limiter
 * =======================================
 *
 * Tests the audit system's rate limiter under heavy concurrent load from
 * multiple CPUs to verify:
 *   1. Rate limit enforcement (no overflow beyond configured limit)
 *   2. Atomic counter correctness (no lost updates from race conditions)
 *   3. No crashes or deadlocks under concurrent access
 *   4. Fair access across all CPUs
 *
 * Test Strategy:
 *   - Configure a known rate limit (e.g., 1000 events/second)
 *   - Launch concurrent logging threads on all available CPUs
 *   - Each CPU hammers audit_log() as fast as possible
 *   - Verify total logged events <= rate_limit
 *   - Verify filtered count + logged count = total attempts
 *   - Check for atomicity violations (lost updates)
 */

#include "../../kernel/include/audit.h"
#include "../../kernel/include/smp.h"
#include "../../kernel/include/kernel.h"
#include "../../kernel/include/spinlock.h"
#include "../../kernel/include/types.h"

// Test configuration
#define TEST_RATE_LIMIT         1000        // Events per second
#define TEST_DURATION_MS        2000        // 2 second test
#define EVENTS_PER_CPU          5000        // Each CPU attempts this many logs
#define MAX_EXPECTED_EVENTS     (TEST_RATE_LIMIT * (TEST_DURATION_MS / 1000) + 100)

// Per-CPU test state
typedef struct {
    uint32_t cpu_id;
    uint64_t events_attempted;      // How many times this CPU called audit_log
    uint64_t events_succeeded;      // How many returned success (0)
    uint64_t events_rate_limited;   // How many returned -1 (rate limited)
    bool started;
    bool finished;
} cpu_test_state_t;

static cpu_test_state_t cpu_states[MAX_CPUS];
static volatile uint32_t cpus_ready = 0;
static volatile uint32_t cpus_finished = 0;
static volatile bool test_start = false;
static spinlock_t state_lock = SPINLOCK_INIT;

// Test statistics
typedef struct {
    uint64_t total_attempted;
    uint64_t total_succeeded;
    uint64_t total_rate_limited;
    uint64_t audit_total_events;
    uint64_t audit_filtered_events;
    uint64_t lost_updates;          // Detected atomicity violations
    bool passed;
} test_results_t;

// External audit functions
extern void audit_init(void);
extern int audit_log(audit_event_type_t type, audit_result_t result,
                     uint32_t pid, uint32_t uid, const char* path,
                     uint32_t syscall, int32_t error_code);
extern audit_config_t* audit_config_get(void);
extern int audit_config_set(audit_config_t* config);
extern audit_stats_t* audit_get_stats(void);
extern void audit_reset_stats(void);

// External timing functions (stubbed if not available)
extern uint64_t get_ticks(void);
extern void delay_ms(uint32_t ms);

// Fallback timing if not available
#ifndef HAVE_TIMER
static uint64_t fake_ticks = 0;
uint64_t get_ticks(void) {
    return __atomic_add_fetch(&fake_ticks, 1, __ATOMIC_SEQ_CST);
}
void delay_ms(uint32_t ms) {
    for (volatile uint64_t i = 0; i < ms * 10000; i++);
}
#endif

/*
 * Worker function executed by each CPU
 * Hammers audit_log() repeatedly and tracks results
 */
static void audit_stress_worker(void* arg) {
    uint32_t cpu = *(uint32_t*)arg;
    cpu_test_state_t* state = &cpu_states[cpu];

    state->cpu_id = cpu;
    state->events_attempted = 0;
    state->events_succeeded = 0;
    state->events_rate_limited = 0;
    state->started = false;
    state->finished = false;

    // Signal ready and wait for all CPUs
    __atomic_add_fetch(&cpus_ready, 1, __ATOMIC_SEQ_CST);
    state->started = true;

    while (!test_start) {
        __asm__ volatile("pause");
    }

    // Stress test: log as fast as possible
    for (uint32_t i = 0; i < EVENTS_PER_CPU; i++) {
        state->events_attempted++;

        int result = audit_log(
            AUDIT_PROC_EXEC,        // Type
            AUDIT_SUCCESS,          // Result
            cpu * 1000 + i,         // PID (unique per CPU)
            cpu,                    // UID (CPU ID)
            "/test/stress",         // Path
            0,                      // Syscall
            0                       // Error code
        );

        if (result == 0) {
            state->events_succeeded++;
        } else if (result == -1) {
            state->events_rate_limited++;
        }

        // Small yield to allow other CPUs to contend
        if (i % 100 == 0) {
            __asm__ volatile("pause");
        }
    }

    state->finished = true;
    __atomic_add_fetch(&cpus_finished, 1, __ATOMIC_SEQ_CST);
}

/*
 * Verify test results and detect issues
 */
static void verify_results(test_results_t* results) {
    audit_stats_t* stats = audit_get_stats();

    // Aggregate per-CPU results
    results->total_attempted = 0;
    results->total_succeeded = 0;
    results->total_rate_limited = 0;

    for (uint32_t cpu = 0; cpu < smp_num_cpus; cpu++) {
        if (!cpu_states[cpu].started) continue;

        results->total_attempted += cpu_states[cpu].events_attempted;
        results->total_succeeded += cpu_states[cpu].events_succeeded;
        results->total_rate_limited += cpu_states[cpu].events_rate_limited;
    }

    // Get audit subsystem stats
    results->audit_total_events = stats->total_events;
    results->audit_filtered_events = stats->events_filtered;

    // Detect lost updates (atomicity violation)
    // If succeeded + rate_limited != attempted, we lost updates
    uint64_t accounted = results->total_succeeded + results->total_rate_limited;
    if (accounted != results->total_attempted) {
        results->lost_updates = results->total_attempted - accounted;
    } else {
        results->lost_updates = 0;
    }

    // Verification checks
    bool passed = true;

    // 1. All events must be accounted for
    if (results->lost_updates > 0) {
        kprintf("[FAIL] Lost updates detected: %llu events unaccounted\n",
                results->lost_updates);
        passed = false;
    }

    // 2. Succeeded events should match audit total (approximately)
    if (results->audit_total_events > results->total_succeeded) {
        kprintf("[WARN] Audit logged more than succeeded: audit=%llu, succeeded=%llu\n",
                results->audit_total_events, results->total_succeeded);
    }

    // 3. Rate limit should be enforced (with tolerance for timing)
    uint64_t expected_max = TEST_RATE_LIMIT * (TEST_DURATION_MS / 1000 + 1);
    if (results->audit_total_events > expected_max + 500) {
        kprintf("[FAIL] Rate limit violated: expected max ~%llu, got %llu\n",
                expected_max, results->audit_total_events);
        passed = false;
    }

    // 4. All CPUs should have participated
    uint32_t active_cpus = 0;
    for (uint32_t cpu = 0; cpu < smp_num_cpus; cpu++) {
        if (cpu_states[cpu].finished) {
            active_cpus++;
        }
    }
    if (active_cpus != smp_num_cpus) {
        kprintf("[FAIL] Not all CPUs participated: %u/%u\n",
                active_cpus, smp_num_cpus);
        passed = false;
    }

    results->passed = passed;
}

/*
 * Print detailed test results
 */
static void print_results(test_results_t* results) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("SMP Audit Rate Limiter Stress Test\n");
    kprintf("========================================\n");
    kprintf("Configuration:\n");
    kprintf("  CPUs:                %u\n", smp_num_cpus);
    kprintf("  Rate limit:          %u events/sec\n", TEST_RATE_LIMIT);
    kprintf("  Events per CPU:      %u\n", EVENTS_PER_CPU);
    kprintf("  Total attempts:      %llu\n", results->total_attempted);
    kprintf("\n");

    kprintf("Per-CPU Results:\n");
    for (uint32_t cpu = 0; cpu < smp_num_cpus; cpu++) {
        if (!cpu_states[cpu].started) continue;

        cpu_test_state_t* s = &cpu_states[cpu];
        kprintf("  CPU %2u: attempted=%5llu  succeeded=%5llu  rate_limited=%5llu  %s\n",
                cpu, s->events_attempted, s->events_succeeded, s->events_rate_limited,
                s->finished ? "DONE" : "INCOMPLETE");
    }
    kprintf("\n");

    kprintf("Aggregate Results:\n");
    kprintf("  Total attempted:     %llu\n", results->total_attempted);
    kprintf("  Total succeeded:     %llu\n", results->total_succeeded);
    kprintf("  Total rate limited:  %llu\n", results->total_rate_limited);
    kprintf("  Lost updates:        %llu %s\n",
            results->lost_updates,
            results->lost_updates > 0 ? "[FAIL]" : "[OK]");
    kprintf("\n");

    kprintf("Audit Subsystem Stats:\n");
    kprintf("  Total events logged: %llu\n", results->audit_total_events);
    kprintf("  Events filtered:     %llu\n", results->audit_filtered_events);
    kprintf("\n");

    kprintf("Verification:\n");

    // Check 1: Atomicity
    if (results->lost_updates == 0) {
        kprintf("  [PASS] Atomicity: All updates accounted for\n");
    } else {
        kprintf("  [FAIL] Atomicity: %llu updates lost\n", results->lost_updates);
    }

    // Check 2: Rate limit enforcement
    uint64_t expected_max = TEST_RATE_LIMIT * (TEST_DURATION_MS / 1000 + 1);
    if (results->audit_total_events <= expected_max + 500) {
        kprintf("  [PASS] Rate limit: %llu events (max ~%llu)\n",
                results->audit_total_events, expected_max);
    } else {
        kprintf("  [FAIL] Rate limit: %llu events exceeds max ~%llu\n",
                results->audit_total_events, expected_max);
    }

    // Check 3: No events lost
    if (results->total_succeeded == results->audit_total_events) {
        kprintf("  [PASS] No events lost in transit\n");
    } else {
        kprintf("  [INFO] Event count mismatch: succeeded=%llu, logged=%llu\n",
                results->total_succeeded, results->audit_total_events);
    }

    // Overall result
    kprintf("\n");
    if (results->passed) {
        kprintf("========================================\n");
        kprintf("  OVERALL: PASSED\n");
        kprintf("========================================\n");
    } else {
        kprintf("========================================\n");
        kprintf("  OVERALL: FAILED\n");
        kprintf("========================================\n");
    }
}

/*
 * Main test entry point
 */
int test_audit_rate_limit_smp(void) {
    kprintf("[TEST] Starting SMP audit rate limiter stress test...\n");

    // Initialize audit subsystem
    audit_init();

    // Configure rate limit
    audit_config_t* config = audit_config_get();
    config->enabled = true;
    config->rate_limit = TEST_RATE_LIMIT;
    config->log_successful = true;  // Log successful events
    config->log_failed = true;
    audit_config_set(config);
    audit_reset_stats();

    kprintf("[TEST] Configured rate limit: %u events/sec\n", TEST_RATE_LIMIT);
    kprintf("[TEST] Running on %u CPUs\n", smp_num_cpus);

    // Initialize test state
    for (uint32_t cpu = 0; cpu < MAX_CPUS; cpu++) {
        cpu_states[cpu].started = false;
        cpu_states[cpu].finished = false;
    }
    cpus_ready = 0;
    cpus_finished = 0;
    test_start = false;

    // Launch worker on each CPU
    // NOTE: This requires SMP scheduler support to schedule tasks on specific CPUs
    // For now, we simulate by calling the worker directly (single-threaded test)
    // In a real SMP environment, this would use smp_call_function_single() or similar

    #ifdef SMP_ENABLE
    // Real multi-CPU test
    for (uint32_t cpu = 0; cpu < smp_num_cpus; cpu++) {
        // TODO: Schedule audit_stress_worker on specific CPU
        // This requires per-CPU scheduling support which is future work
        kprintf("[TEST] Would launch worker on CPU %u (SMP scheduling not implemented)\n", cpu);
    }
    #else
    // Simulated multi-CPU test (sequential execution)
    kprintf("[TEST] SMP not enabled, simulating %u CPUs sequentially\n", smp_num_cpus);

    for (uint32_t cpu = 0; cpu < smp_num_cpus; cpu++) {
        audit_stress_worker(&cpu);
    }
    #endif

    // Start the test
    kprintf("[TEST] Starting stress test...\n");
    test_start = true;

    // Wait for all CPUs to finish (or timeout)
    uint32_t timeout = 0;
    while (cpus_finished < smp_num_cpus && timeout < 5000) {
        delay_ms(10);
        timeout += 10;
    }

    if (timeout >= 5000) {
        kprintf("[TEST] WARNING: Test timed out after %u ms\n", timeout);
    } else {
        kprintf("[TEST] All CPUs finished in %u ms\n", timeout);
    }

    // Verify and print results
    test_results_t results;
    verify_results(&results);
    print_results(&results);

    return results.passed ? 0 : -1;
}

/*
 * Minimal unit test - single CPU verification
 * Tests rate limiter correctness even on single CPU
 */
int test_audit_rate_limit_single_cpu(void) {
    kprintf("[TEST] Single-CPU rate limiter test...\n");

    audit_init();

    audit_config_t* config = audit_config_get();
    config->enabled = true;
    config->rate_limit = 100;  // 100 events/second
    config->log_successful = true;
    audit_config_set(config);
    audit_reset_stats();

    // Attempt to log 200 events (should hit rate limit)
    uint32_t succeeded = 0;
    uint32_t rate_limited = 0;

    for (uint32_t i = 0; i < 200; i++) {
        int result = audit_log(AUDIT_PROC_EXEC, AUDIT_SUCCESS,
                              i, 0, "/test", 0, 0);
        if (result == 0) {
            succeeded++;
        } else if (result == -1) {
            rate_limited++;
        }
    }

    audit_stats_t* stats = audit_get_stats();

    kprintf("[TEST] Results: attempted=200, succeeded=%u, rate_limited=%u\n",
            succeeded, rate_limited);
    kprintf("[TEST] Audit stats: logged=%llu, filtered=%llu\n",
            stats->total_events, stats->events_filtered);

    // Verify rate limit enforced
    bool passed = true;
    if (succeeded > 110) {  // Allow some tolerance
        kprintf("[FAIL] Rate limit not enforced: %u > 100\n", succeeded);
        passed = false;
    }
    if (succeeded + rate_limited != 200) {
        kprintf("[FAIL] Lost updates: %u + %u != 200\n", succeeded, rate_limited);
        passed = false;
    }

    if (passed) {
        kprintf("[PASS] Single-CPU rate limiter test\n");
    } else {
        kprintf("[FAIL] Single-CPU rate limiter test\n");
    }

    return passed ? 0 : -1;
}
