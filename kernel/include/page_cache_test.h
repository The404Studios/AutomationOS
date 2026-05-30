/**
 * Page Cache Benchmark Tests
 */

#ifndef PAGE_CACHE_TEST_H
#define PAGE_CACHE_TEST_H

/* Run page cache benchmark (1000 repeated reads) */
void page_cache_benchmark(void);

/* Run page cache stress test (many small files) */
void page_cache_stress_test(void);

#endif // PAGE_CACHE_TEST_H
