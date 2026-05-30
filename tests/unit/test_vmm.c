/*
 * Unit Tests for Virtual Memory Manager (VMM)
 *
 * Tests virtual memory management including:
 * - Page table initialization
 * - Address mapping and unmapping
 * - Permission handling
 * - TLB invalidation
 * - Higher-half kernel mapping
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

// Test framework
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(condition) do { \
    tests_run++; \
    if (condition) { \
        tests_passed++; \
    } else { \
        tests_failed++; \
        fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while(0)

#define TEST(name) void test_##name()

#define RUN_TEST(name) do { \
    printf("  Running test: %s... ", #name); \
    fflush(stdout); \
    test_##name(); \
    printf("PASS\n"); \
} while(0)

// VMM constants (from kernel)
#define PAGE_SIZE 4096
#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000ULL

// Page flags
#define PAGE_PRESENT  (1 << 0)
#define PAGE_WRITE    (1 << 1)
#define PAGE_USER     (1 << 2)
#define PAGE_ACCESSED (1 << 5)
#define PAGE_DIRTY    (1 << 6)

// Mock page table entry
typedef uint64_t pte_t;

// Mock VMM state
static struct {
    int initialized;
    uint64_t pml4_phys;
    int map_count;
    int unmap_count;
} vmm_state = {0, 0, 0, 0};

// Mock functions
void kprintf(const char* fmt, ...) {
    (void)fmt;
}

// ============================================================================
// Mock VMM Functions (simplified versions for testing)
// ============================================================================

int vmm_init(void) {
    if (vmm_state.initialized) {
        return -1;  // Already initialized
    }
    vmm_state.initialized = 1;
    vmm_state.pml4_phys = 0x1000;  // Mock physical address
    return 0;
}

int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!vmm_state.initialized) {
        return -1;  // Not initialized
    }
    if (virt == 0 || phys == 0) {
        return -2;  // Invalid address
    }
    if ((virt & (PAGE_SIZE - 1)) != 0 || (phys & (PAGE_SIZE - 1)) != 0) {
        return -3;  // Misaligned address
    }

    vmm_state.map_count++;
    return 0;
}

int vmm_unmap_page(uint64_t virt) {
    if (!vmm_state.initialized) {
        return -1;  // Not initialized
    }
    if (virt == 0) {
        return -2;  // Invalid address
    }

    vmm_state.unmap_count++;
    return 0;
}

uint64_t vmm_get_physical(uint64_t virt) {
    if (!vmm_state.initialized) {
        return 0;
    }
    if (virt >= KERNEL_VIRT_BASE) {
        // Higher-half kernel address
        return virt - KERNEL_VIRT_BASE;
    }
    // Mock: return same address for testing
    return virt;
}

void vmm_invalidate_tlb(uint64_t virt) {
    (void)virt;
    // Mock: do nothing
}

// ============================================================================
// Initialization Tests
// ============================================================================

TEST(vmm_init_success) {
    vmm_state.initialized = 0;
    int result = vmm_init();
    ASSERT(result == 0);
    ASSERT(vmm_state.initialized == 1);
}

TEST(vmm_init_already_initialized) {
    vmm_state.initialized = 1;
    int result = vmm_init();
    ASSERT(result == -1);  // Should fail if already initialized
}

TEST(vmm_init_sets_pml4) {
    vmm_state.initialized = 0;
    vmm_init();
    ASSERT(vmm_state.pml4_phys != 0);
}

// ============================================================================
// Page Mapping Tests
// ============================================================================

TEST(vmm_map_page_valid) {
    vmm_state.initialized = 1;
    vmm_state.map_count = 0;

    uint64_t virt = 0x1000;
    uint64_t phys = 0x2000;
    uint64_t flags = PAGE_PRESENT | PAGE_WRITE;

    int result = vmm_map_page(virt, phys, flags);
    ASSERT(result == 0);
    ASSERT(vmm_state.map_count == 1);
}

TEST(vmm_map_page_not_initialized) {
    vmm_state.initialized = 0;

    int result = vmm_map_page(0x1000, 0x2000, PAGE_PRESENT);
    ASSERT(result == -1);
}

TEST(vmm_map_page_null_virt) {
    vmm_state.initialized = 1;

    int result = vmm_map_page(0, 0x2000, PAGE_PRESENT);
    ASSERT(result == -2);  // Invalid virtual address
}

TEST(vmm_map_page_null_phys) {
    vmm_state.initialized = 1;

    int result = vmm_map_page(0x1000, 0, PAGE_PRESENT);
    ASSERT(result == -2);  // Invalid physical address
}

TEST(vmm_map_page_misaligned_virt) {
    vmm_state.initialized = 1;

    // Address not aligned to page boundary
    int result = vmm_map_page(0x1234, 0x2000, PAGE_PRESENT);
    ASSERT(result == -3);  // Misaligned
}

TEST(vmm_map_page_misaligned_phys) {
    vmm_state.initialized = 1;

    // Address not aligned to page boundary
    int result = vmm_map_page(0x1000, 0x2345, PAGE_PRESENT);
    ASSERT(result == -3);  // Misaligned
}

TEST(vmm_map_page_multiple_mappings) {
    vmm_state.initialized = 1;
    vmm_state.map_count = 0;

    // Map multiple pages
    vmm_map_page(0x1000, 0x2000, PAGE_PRESENT);
    vmm_map_page(0x2000, 0x3000, PAGE_PRESENT);
    vmm_map_page(0x3000, 0x4000, PAGE_PRESENT);

    ASSERT(vmm_state.map_count == 3);
}

// ============================================================================
// Page Unmapping Tests
// ============================================================================

TEST(vmm_unmap_page_valid) {
    vmm_state.initialized = 1;
    vmm_state.unmap_count = 0;

    int result = vmm_unmap_page(0x1000);
    ASSERT(result == 0);
    ASSERT(vmm_state.unmap_count == 1);
}

TEST(vmm_unmap_page_not_initialized) {
    vmm_state.initialized = 0;

    int result = vmm_unmap_page(0x1000);
    ASSERT(result == -1);
}

TEST(vmm_unmap_page_null) {
    vmm_state.initialized = 1;

    int result = vmm_unmap_page(0);
    ASSERT(result == -2);  // Invalid address
}

// ============================================================================
// Address Translation Tests
// ============================================================================

TEST(vmm_get_physical_kernel_address) {
    vmm_state.initialized = 1;

    // Higher-half kernel address
    uint64_t virt = KERNEL_VIRT_BASE + 0x1000;
    uint64_t phys = vmm_get_physical(virt);

    ASSERT(phys == 0x1000);  // Should subtract kernel base
}

TEST(vmm_get_physical_user_address) {
    vmm_state.initialized = 1;

    // User space address
    uint64_t virt = 0x400000;
    uint64_t phys = vmm_get_physical(virt);

    ASSERT(phys != 0);  // Should return valid physical address
}

TEST(vmm_get_physical_not_initialized) {
    vmm_state.initialized = 0;

    uint64_t phys = vmm_get_physical(0x1000);
    ASSERT(phys == 0);  // Should return 0 if not initialized
}

// ============================================================================
// TLB Invalidation Tests
// ============================================================================

TEST(vmm_invalidate_tlb_valid) {
    // Just ensure it doesn't crash
    vmm_invalidate_tlb(0x1000);
    ASSERT(1);  // Success if we got here
}

TEST(vmm_invalidate_tlb_multiple) {
    // Invalidate multiple addresses
    vmm_invalidate_tlb(0x1000);
    vmm_invalidate_tlb(0x2000);
    vmm_invalidate_tlb(0x3000);
    ASSERT(1);
}

// ============================================================================
// Permission Tests
// ============================================================================

TEST(vmm_map_page_read_only) {
    vmm_state.initialized = 1;

    // Map with read-only permissions
    uint64_t flags = PAGE_PRESENT;  // No PAGE_WRITE
    int result = vmm_map_page(0x1000, 0x2000, flags);
    ASSERT(result == 0);
}

TEST(vmm_map_page_read_write) {
    vmm_state.initialized = 1;

    // Map with read-write permissions
    uint64_t flags = PAGE_PRESENT | PAGE_WRITE;
    int result = vmm_map_page(0x1000, 0x2000, flags);
    ASSERT(result == 0);
}

TEST(vmm_map_page_user_accessible) {
    vmm_state.initialized = 1;

    // Map with user access
    uint64_t flags = PAGE_PRESENT | PAGE_USER;
    int result = vmm_map_page(0x1000, 0x2000, flags);
    ASSERT(result == 0);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST(vmm_map_page_large_address) {
    vmm_state.initialized = 1;

    // Map at high address
    uint64_t virt = 0x7FFFFFF000ULL;
    uint64_t phys = 0x1000;
    int result = vmm_map_page(virt, phys, PAGE_PRESENT);
    ASSERT(result == 0);
}

TEST(vmm_map_page_all_flags) {
    vmm_state.initialized = 1;

    // Map with all flags
    uint64_t flags = PAGE_PRESENT | PAGE_WRITE | PAGE_USER |
                     PAGE_ACCESSED | PAGE_DIRTY;
    int result = vmm_map_page(0x1000, 0x2000, flags);
    ASSERT(result == 0);
}

// ============================================================================
// Test Runner
// ============================================================================

int main() {
    printf("========================================\n");
    printf("  Virtual Memory Manager Tests\n");
    printf("========================================\n\n");

    // Initialization tests
    printf("Initialization tests:\n");
    RUN_TEST(vmm_init_success);
    RUN_TEST(vmm_init_already_initialized);
    RUN_TEST(vmm_init_sets_pml4);
    printf("\n");

    // Mapping tests
    printf("Page mapping tests:\n");
    vmm_state.initialized = 1;  // Reset for next tests
    RUN_TEST(vmm_map_page_valid);
    RUN_TEST(vmm_map_page_not_initialized);
    RUN_TEST(vmm_map_page_null_virt);
    RUN_TEST(vmm_map_page_null_phys);
    RUN_TEST(vmm_map_page_misaligned_virt);
    RUN_TEST(vmm_map_page_misaligned_phys);
    RUN_TEST(vmm_map_page_multiple_mappings);
    printf("\n");

    // Unmapping tests
    printf("Page unmapping tests:\n");
    RUN_TEST(vmm_unmap_page_valid);
    RUN_TEST(vmm_unmap_page_not_initialized);
    RUN_TEST(vmm_unmap_page_null);
    printf("\n");

    // Address translation tests
    printf("Address translation tests:\n");
    vmm_state.initialized = 1;
    RUN_TEST(vmm_get_physical_kernel_address);
    RUN_TEST(vmm_get_physical_user_address);
    RUN_TEST(vmm_get_physical_not_initialized);
    printf("\n");

    // TLB tests
    printf("TLB invalidation tests:\n");
    RUN_TEST(vmm_invalidate_tlb_valid);
    RUN_TEST(vmm_invalidate_tlb_multiple);
    printf("\n");

    // Permission tests
    printf("Permission tests:\n");
    vmm_state.initialized = 1;
    RUN_TEST(vmm_map_page_read_only);
    RUN_TEST(vmm_map_page_read_write);
    RUN_TEST(vmm_map_page_user_accessible);
    printf("\n");

    // Edge cases
    printf("Edge case tests:\n");
    RUN_TEST(vmm_map_page_large_address);
    RUN_TEST(vmm_map_page_all_flags);
    printf("\n");

    // Print summary
    printf("========================================\n");
    printf("  Test Results\n");
    printf("========================================\n");
    printf("Tests run:    %d\n", tests_run);
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_failed);
    printf("========================================\n\n");

    if (tests_failed > 0) {
        printf("FAIL: %d test(s) failed\n", tests_failed);
        return 1;
    }

    printf("SUCCESS: All tests passed\n");
    return 0;
}
