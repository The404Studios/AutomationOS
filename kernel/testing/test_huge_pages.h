#ifndef TEST_HUGE_PAGES_H
#define TEST_HUGE_PAGES_H

/**
 * Huge Page Test Suite
 *
 * Tests 2MB huge page allocation, mapping, and TLB performance.
 */

// Run all huge page tests
void run_huge_page_tests(void);

// Individual test functions
void test_huge_page_allocator(void);
void test_huge_page_mapping(void);
void test_huge_pages_performance(void);

#endif
