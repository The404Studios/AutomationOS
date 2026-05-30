# 2MB Huge Pages Implementation for AutomationOS

## Overview

This implementation adds support for 2MB huge pages to reduce TLB (Translation Lookaside Buffer) pressure by up to 512x for large memory allocations.

## What Was Implemented

### 1. Huge Page Allocator (pmm.c)

- **`pmm_alloc_huge_page()`**: Allocates a 2MB physically contiguous, 2MB-aligned page
- **`pmm_free_huge_page()`**: Frees a 2MB huge page
- **`pmm_get_huge_page_stats()`**: Returns allocation statistics
- Internally uses `pmm_alloc_pages(512)` to get contiguous 4KB pages

### 2. Page Table Support (paging.c)

- **`paging_map_huge_page()`**: Maps a 2MB page using PSE (Page Size Extension) bit 7 in PDE
- **`paging_unmap_huge_page()`**: Unmaps a 2MB huge page
- Validates 2MB alignment (bits 0-20 must be zero)
- Sets PS bit (bit 7) in Page Directory Entry to signal huge page
- Single TLB invalidation covers entire 2MB range

### 3. Testing & Benchmarking

- **Unit tests**: `kernel/testing/test_huge_pages.c`
  - Tests allocator correctness
  - Validates mapping/unmapping
  - Performance comparison vs 4KB pages
  
- **TLB benchmark**: `tests/bench/bench_huge_pages.c`
  - Measures TLB pressure with 100MB allocation
  - Random access pattern to maximize TLB misses
  - Compares 4KB vs 2MB page performance

## Performance Impact

### TLB Entry Reduction

| Allocation Size | 4KB Pages | 2MB Pages | Reduction |
|-----------------|-----------|-----------|-----------|
| 100MB           | 25,600    | 50        | 512x      |
| 1GB             | 262,144   | 512       | 512x      |

### Expected Performance

- **4KB pages**: 25,600 TLB entries needed → HIGH TLB miss rate (CPU TLB ~1,536 entries)
- **2MB pages**: 50 TLB entries needed → MINIMAL TLB misses (fits in CPU TLB)
- **Improvement**: 99%+ reduction in TLB misses for large allocations

## Integration Guide

### Step 1: Build

The implementation is integrated into existing memory management. Rebuild kernel:

```bash
cd /path/to/Kernel
make clean
make
```

### Step 2: Run Tests

Add to your kernel initialization (e.g., `kernel/main.c`):

```c
#include "testing/test_huge_pages.h"

void kernel_main(void) {
    // ... existing init code ...
    
    // Run huge page tests
    run_huge_page_tests();
    
    // ... rest of kernel ...
}
```

### Step 3: Run Benchmark

Add to benchmark suite:

```c
#include "tests/bench/bench_huge_pages.h"

void run_benchmarks(void) {
    // ... existing benchmarks ...
    
    // Run TLB benchmark
    bench_huge_pages_comparison();
}
```

### Step 4: Use in Your Code

```c
#include "include/mem.h"

// Allocate a 2MB huge page
void* huge_page = pmm_alloc_huge_page();
if (!huge_page) {
    kprintf("Failed to allocate huge page\n");
    return;
}

// Map it (virt and phys must be 2MB-aligned)
void* vaddr = (void*)0xFFFFFF8000000000ULL;  // Example kernel address
int ret = paging_map_huge_page(vaddr, huge_page, PAGE_PRESENT | PAGE_WRITE);
if (ret != 0) {
    kprintf("Failed to map huge page\n");
    pmm_free_huge_page(huge_page);
    return;
}

// Use the memory
uint8_t* ptr = (uint8_t*)vaddr;
for (size_t i = 0; i < 2 * 1024 * 1024; i++) {
    ptr[i] = (uint8_t)i;
}

// Cleanup
paging_unmap_huge_page(vaddr);
pmm_free_huge_page(huge_page);
```

## Transparent Huge Pages (Future Enhancement)

To automatically use huge pages for large `mmap` allocations, modify `vmm_mmap_anon()`:

```c
void* vmm_mmap_anon(uint64_t cr3, uint64_t len, uint32_t prot) {
    // Check if allocation is 2MB-aligned and >= 2MB
    if (len >= 2*1024*1024 && !(len & 0x1FFFFF)) {
        // Try to allocate as huge pages
        size_t num_huge_pages = len / (2*1024*1024);
        
        for (size_t i = 0; i < num_huge_pages; i++) {
            void* phys = pmm_alloc_huge_page();
            if (!phys) {
                // Fall back to 4KB pages
                goto use_small_pages;
            }
            
            void* vaddr = allocate_vaddr_2mb_aligned();
            paging_map_huge_page(vaddr, phys, flags);
        }
        
        return vaddr_base;
    }
    
use_small_pages:
    // Existing 4KB page allocation logic
    // ...
}
```

## API Reference

### Physical Memory Manager

```c
// Allocate a 2MB huge page (512 contiguous 4KB pages, 2MB-aligned)
void* pmm_alloc_huge_page(void);

// Free a 2MB huge page
void pmm_free_huge_page(void* base);

// Get allocation statistics
void pmm_get_huge_page_stats(uint64_t* allocated, uint64_t* freed, uint64_t* failures);
```

### Virtual Memory Manager

```c
// Map a 2MB huge page (virt and phys must be 2MB-aligned)
int paging_map_huge_page(void* virt, void* phys, uint64_t flags);

// Unmap a 2MB huge page (virt must be 2MB-aligned)
int paging_unmap_huge_page(void* virt);
```

### Page Flags

```c
PAGE_PRESENT    // Bit 0: Page is present in memory
PAGE_WRITE      // Bit 1: Page is writable
PAGE_USER       // Bit 2: Page is accessible from user mode
PAGE_NX         // Bit 63: No-Execute (when EFER.NXE is set)
```

## Technical Details

### Page Directory Entry Format (2MB Huge Page)

```
Bits 0-11:  Flags (PRESENT, WRITE, USER, etc.)
Bits 12-20: Must be 0 (2MB alignment requirement)
Bits 21-51: Physical address (2MB-aligned)
Bit 7:      PS (Page Size) = 1 for huge page
Bit 63:     NX (No-Execute)
```

### TLB Behavior

- **4KB page**: Each page requires one TLB entry
- **2MB huge page**: One TLB entry covers entire 2MB (512x efficiency)
- **invlpg instruction**: Invalidates entire huge page with single invlpg

### Alignment Requirements

- Physical address: Must be 2MB-aligned (bits 0-20 = 0)
- Virtual address: Must be 2MB-aligned (bits 0-20 = 0)
- Function validates alignment and returns -1 on error

## Testing Results

Run the test suite to verify:

```bash
# Expected output:
[TEST] Huge Page Allocator
===========================
[TEST] PASS: Allocated huge page at 0x...
[TEST] PASS: Huge page is 2MB-aligned
[TEST] PASS: Huge page memory access works
...

[BENCH] 4KB Pages - TLB Pressure Test
======================================
Total pages: 25,600 (requires 25,600 TLB entries)
TLB misses expected: HIGH

[BENCH] 2MB Huge Pages - TLB Pressure Test
==========================================
Total huge pages: 50 (requires 50 TLB entries)
TLB misses expected: LOW
```

## Files Modified/Created

### Modified
- `kernel/core/mem/pmm.c` - Added huge page allocator
- `kernel/arch/x86_64/paging.c` - Added huge page mapping functions
- `kernel/include/mem.h` - Added huge page API declarations

### Created
- `kernel/testing/test_huge_pages.c` - Unit tests
- `kernel/testing/test_huge_pages.h` - Test header
- `tests/bench/bench_huge_pages.c` - TLB benchmark
- `tests/bench/bench_huge_pages.h` - Benchmark header
- `HUGE_PAGES_IMPLEMENTATION.md` - This document

## Known Limitations

1. **Manual allocation only**: No automatic transparent huge pages (THP) yet
2. **No fallback**: If huge page allocation fails, caller must handle fallback to 4KB pages
3. **No splitting**: Cannot split a 2MB huge page into 4KB pages dynamically
4. **No merging**: Cannot merge 512 contiguous 4KB pages into a huge page

## Future Enhancements

1. **Transparent Huge Pages**: Automatically use huge pages for large `mmap()`
2. **Defragmentation**: Compact memory to create more 2MB-aligned runs
3. **1GB pages**: Support for 1GB pages (bit 7 in PDPT entry)
4. **Performance counters**: Use PMU to measure actual TLB misses
5. **NUMA awareness**: Allocate huge pages from local NUMA node
6. **Khugepaged**: Background daemon to promote eligible 4KB regions to huge pages

## References

- Intel® 64 and IA-32 Architectures Software Developer's Manual, Volume 3A, Chapter 4
- Page Size Extension (PSE) - IA-32 bit 7 in Page Directory Entry
- Linux Transparent Huge Pages documentation

## Conclusion

This implementation provides a solid foundation for huge page support in AutomationOS. The 512x reduction in TLB pressure for large allocations should significantly improve performance for memory-intensive workloads.

Measure the TLB impact with the included benchmark to quantify the performance improvement on your hardware.
