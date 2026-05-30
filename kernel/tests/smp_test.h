/*
 * SMP Test Suite Header
 * =====================
 *
 * Include this header to run SMP validation tests during kernel boot.
 */

#ifndef SMP_TEST_H
#define SMP_TEST_H

#include "../include/types.h"

/*
 * Run comprehensive SMP validation test suite.
 *
 * Tests:
 * 1. CPU Detection - Verify ACPI MADT parsing and CPU discovery
 * 2. AP Startup - Ensure all CPUs boot successfully
 * 3. Per-CPU Data - Verify data isolation between CPUs
 * 4. IPI Delivery - Test inter-processor interrupts
 * 5. IPI Latency - Measure IPI round-trip time
 * 6. TLB Shootdown - Verify TLB synchronization
 * 7. Cache Coherence - Test MESI protocol and atomics
 * 8. Performance Scaling - Measure parallel efficiency
 * 9. Stress Test - Run all CPUs at 100% for stability
 *
 * Call this after smp_init(), smp_start_aps(), and ipi_init().
 */
void smp_run_tests(void);

/*
 * Run quick SMP sanity check (subset of full tests).
 * Useful for regular boot without full validation.
 */
void smp_sanity_check(void);

#endif // SMP_TEST_H
