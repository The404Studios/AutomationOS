/*
 * test_rapid_cpu1.c -- Kernel-space rapid CPU1 offload stress test
 * =================================================================
 *
 * STRESS TEST: 100 rapid-fire CPU1 offloads to check for races.
 *
 * Tests:
 *   1. NO OWNERSHIP ASSERTION PANICS: own_transition() survives slot reuse.
 *   2. NO UAF CRASHES: No use-after-free on job slot.
 *   3. JOB SLOT LOCK PREVENTS CLOBBERING: cpu1_job_lock serializes writes.
 *   4. ALL JOBS COMPLETE: Every iteration gets a valid result.
 *
 * Called from kernel.c after SMP init (only if SMP_FOUNDATION is enabled).
 */

#ifdef SMP_FOUNDATION

#include "../../include/types.h"
#include "../../include/kernel.h"
#include "../../include/perf.h"   /* static inline rdtsc() — there is no global
                                    rdtsc symbol; the extern below would not link */

/* Forward declarations from ap_boot.c */
extern int cpu1_submit(void (*fn)(void *), void *arg);
extern int cpu1_wait(uint64_t deadline_tsc);

#define TEST_ITERATIONS 100
#define TSC_PER_US      3000ULL
#define TIMEOUT_US      100000ULL  /* 100 ms per job */

/* Simple test job: increment a counter. */
static volatile uint64_t g_test_counter = 0;

static void test_increment_job(void *arg) {
    uint64_t *counter = (uint64_t *)arg;
    __atomic_add_fetch(counter, 1, __ATOMIC_SEQ_CST);
}

/*
 * test_rapid_cpu1_offload -- Run 100 rapid offloads sequentially.
 *
 * Returns:
 *   - Number of successful offloads (0-100).
 *   - 100 = PASS, <100 = FAIL (prints first failure).
 */
int test_rapid_cpu1_offload(void) {
    int success_count = 0;
    int first_failure = -1;

    kprintf("[RAPID_CPU1] Starting rapid-fire stress test (%d iterations)\n", TEST_ITERATIONS);

    for (int i = 0; i < TEST_ITERATIONS; i++) {
        uint64_t expected = g_test_counter + 1;
        uint64_t deadline = rdtsc() + (TIMEOUT_US * TSC_PER_US);

        /* Submit job */
        if (!cpu1_submit(test_increment_job, (void *)&g_test_counter)) {
            kprintf("[RAPID_CPU1] Iter %d: submit failed (CPU1 offline?)\n", i);
            if (first_failure < 0) first_failure = i;
            continue;
        }

        /* Wait for completion */
        int rc = cpu1_wait(deadline);
        if (rc == 0) {
            kprintf("[RAPID_CPU1] Iter %d: timeout\n", i);
            if (first_failure < 0) first_failure = i;
            continue;
        } else if (rc < 0) {
            kprintf("[RAPID_CPU1] Iter %d: wait failed rc=%d\n", i, rc);
            if (first_failure < 0) first_failure = i;
            continue;
        }

        /* Verify result */
        uint64_t actual = __atomic_load_n(&g_test_counter, __ATOMIC_SEQ_CST);
        if (actual != expected) {
            kprintf("[RAPID_CPU1] Iter %d: mismatch (expected %llu, got %llu)\n",
                    i, expected, actual);
            if (first_failure < 0) first_failure = i;
            continue;
        }

        success_count++;

        /* Progress report every 10 iterations */
        if ((i + 1) % 10 == 0) {
            kprintf("[RAPID_CPU1] Progress: %d/%d (%d successful)\n",
                    i + 1, TEST_ITERATIONS, success_count);
        }
    }

    /* Final report */
    if (success_count == TEST_ITERATIONS) {
        kprintf("[RAPID_CPU1] PASS: %d/%d successful\n", success_count, TEST_ITERATIONS);
    } else {
        kprintf("[RAPID_CPU1] FAIL: %d/%d successful (first failure at iteration %d)\n",
                success_count, TEST_ITERATIONS, first_failure);
    }

    return success_count;
}

#endif  /* SMP_FOUNDATION */
