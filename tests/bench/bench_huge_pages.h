#ifndef BENCH_HUGE_PAGES_H
#define BENCH_HUGE_PAGES_H

/**
 * Huge Page TLB Benchmark
 *
 * Measures TLB miss reduction with 2MB huge pages.
 */

// Run TLB comparison benchmark (4KB vs 2MB pages)
void bench_huge_pages_comparison(void);

// Individual benchmarks
void bench_4kb_pages(void);
void bench_2mb_huge_pages(void);
void bench_transparent_huge_pages(void);

#endif
