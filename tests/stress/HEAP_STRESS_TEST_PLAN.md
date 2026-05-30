# Heap Allocator Stress Test Plan

## Executive Summary

This document outlines a comprehensive stress testing plan for the kernel heap allocator (`kernel/core/mem/heap.c`). The plan identifies critical edge cases, validates safety mechanisms, and tests the allocator under extreme allocation patterns.

**Target:** AutomationOS kernel heap allocator (16MB, first-fit with coalescing)  
**Location:** `C:\Users\wilde\Desktop\Kernel\kernel\core\mem\heap.c`  
**Test Framework:** `tests/stress/heap_stress_test.c` (to be implemented)

---

## 1. Heap Implementation Analysis

### 1.1 Architecture

```c
// Heap Configuration
#define HEAP_START 0xFFFFFFFF90000000ULL
#define HEAP_SIZE  (16 * 1024 * 1024)  // 16 MB
#define ALIGNMENT  16                   // 16-byte alignment

// Block Structure
typedef struct block {
    size_t size;           // Size of usable data region
    bool is_free;          // Allocation status
    struct block* next;    // Next block in free list
} block_t;
```

### 1.2 Key Operations

**Allocation (`kmalloc`)**
1. Validate size != 0 (returns NULL)
2. Align size to 16 bytes using `ALIGN_UP(size, 16)`
3. Acquire global heap lock (spinlock)
4. First-fit search for free block >= size
5. Split block if remainder > (sizeof(block_t) + 16)
6. Mark block as allocated
7. Update heap_used counter
8. Release lock
9. Return pointer (block + sizeof(block_t))

**Deallocation (`kfree`)**
1. Validate ptr != NULL (returns early)
2. Validate ptr is within heap bounds via `heap_owns(ptr)`
3. Acquire global heap lock
4. Get block header (ptr - sizeof(block_t))
5. Mark block as free
6. Update heap_used counter
7. Coalesce with next block if free
8. Coalesce with previous block if free
9. Release lock

**Validation (`heap_owns`)**
- Returns true if: `HEAP_START <= ptr < HEAP_START + HEAP_SIZE`
- Protects against freeing stack/static/initrd addresses

### 1.3 Current Safety Mechanisms

**Implemented (BUG-007, BUG-017 fixes):**
- Overflow check during block splitting (prevents wraparound)
- `heap_owns()` validation in `kfree()` (rejects non-heap pointers)
- Block validation in `kfree()` (detects double-free via linked list)
- NULL pointer handling in `kfree()`
- Zero-size allocation handling in `kmalloc()`

**Missing (identified in STRESS_TEST_REPORT.md):**
- No magic number validation (heap block integrity)
- No canary values (buffer overflow detection)
- No per-block free status validation (double-free mitigation)
- OOM handling panics instead of returning NULL
- Integer overflow protection for alignment

---

## 2. Critical Edge Cases to Test

### 2.1 Allocation Edge Cases

#### Test 2.1.1: Zero-Size Allocation
```c
void* ptr = kmalloc(0);
assert(ptr == NULL);  // Must return NULL per spec
```
**Expected:** NULL  
**Status:** Implemented (line 74 in heap.c)

#### Test 2.1.2: Massive Allocation (Beyond Heap Size)
```c
void* ptr = kmalloc(HEAP_SIZE + 1);
// Currently: kernel_panic("Heap out of memory")
// Desired: return NULL
```
**Expected:** NULL or panic (document current behavior)  
**Status:** Panics on OOM (line 124 in heap.c) - **HIGH severity**

#### Test 2.1.3: Exact Heap Size Allocation
```c
void* ptr = kmalloc(HEAP_SIZE - sizeof(block_t));
assert(ptr != NULL);  // Should succeed (entire heap as one block)
```
**Expected:** Success  
**Test:** Validate entire heap is allocated

#### Test 2.1.4: Single Byte Allocation (Alignment Test)
```c
void* ptr = kmalloc(1);
assert(ptr != NULL);
assert(((uint64_t)ptr & 0xF) == 0);  // 16-byte aligned
// Block metadata should show size = 16 (aligned)
```
**Expected:** 16-byte aligned pointer, block size = 16

#### Test 2.1.5: Odd-Size Allocation (17 bytes)
```c
void* ptr = kmalloc(17);
assert(ptr != NULL);
assert(((uint64_t)ptr & 0xF) == 0);  // 16-byte aligned
// Block should have size = 32 (17 rounded up to 16, then aligned)
```
**Expected:** 16-byte aligned, block size = 32

#### Test 2.1.6: Integer Overflow in Alignment
```c
size_t evil_size = (size_t)-8;  // SIZE_MAX - 7
void* ptr = kmalloc(evil_size);
// ALIGN_UP(evil_size, 16) could wrap to small value!
```
**Expected:** NULL (overflow protection)  
**Status:** **MEDIUM severity** - No overflow protection identified

---

### 2.2 Deallocation Edge Cases

#### Test 2.2.1: NULL Pointer Free
```c
kfree(NULL);
// Must be no-op
```
**Expected:** Safe no-op  
**Status:** Implemented (line 137 in heap.c)

#### Test 2.2.2: Double Free Detection
```c
void* ptr = kmalloc(128);
kfree(ptr);
kfree(ptr);  // Second free - should be detected!
```
**Expected:** Panic with "double free or corruption" message  
**Status:** Implemented via linked list validation (line 165-168 in heap.c)  
**Weakness:** Not as robust as magic number validation

#### Test 2.2.3: Free of Stack Address
```c
int stack_var = 42;
kfree(&stack_var);  // Invalid pointer
// heap_owns() should reject this
```
**Expected:** Silent rejection (returns early, line 140-144)  
**Status:** Implemented via `heap_owns()` check

#### Test 2.2.4: Free of Static Data Address
```c
static int static_var = 42;
kfree(&static_var);
```
**Expected:** Silent rejection via `heap_owns()`  
**Status:** Implemented

#### Test 2.2.5: Free of Initrd Address
```c
extern char _initrd_start[];
kfree(_initrd_start);
```
**Expected:** Silent rejection via `heap_owns()`  
**Status:** Implemented

#### Test 2.2.6: Free of Kernel Text Address
```c
kfree((void*)kernel_main);  // Function pointer
```
**Expected:** Silent rejection  
**Status:** Implemented

---

### 2.3 Stress Patterns

#### Test 2.3.1: Many Small Allocations (Fragmentation Test)
```c
#define NUM_SMALL_ALLOCS 100
void* ptrs[NUM_SMALL_ALLOCS];

for (int i = 0; i < NUM_SMALL_ALLOCS; i++) {
    ptrs[i] = kmalloc(16);
    if (!ptrs[i]) {
        kprintf("[FAIL] Allocation %d failed (heap may be fragmented)\n", i);
        break;
    }
}

// Verify all allocations succeeded
int successful = 0;
for (int i = 0; i < NUM_SMALL_ALLOCS; i++) {
    if (ptrs[i]) successful++;
}

kprintf("[INFO] Successfully allocated %d/%d small blocks\n", 
        successful, NUM_SMALL_ALLOCS);

// Cleanup
for (int i = 0; i < NUM_SMALL_ALLOCS; i++) {
    if (ptrs[i]) kfree(ptrs[i]);
}
```
**Expected:** All 100 allocations succeed  
**Failure Mode:** Premature OOM due to fragmentation

#### Test 2.3.2: Large Allocation Test
```c
void* large = kmalloc(1024 * 1024);  // 1MB
if (!large) {
    kprintf("[FAIL] Cannot allocate 1MB block\n");
} else {
    kprintf("[PASS] 1MB allocation successful\n");
    kfree(large);
}
```
**Expected:** Success (heap is 16MB)  
**Test:** Verify contiguous space available

#### Test 2.3.3: Fragmentation Pattern (Checkerboard)
```c
// Allocate alternating blocks
void* ptrs[50];
for (int i = 0; i < 50; i++) {
    ptrs[i] = kmalloc(128);
    assert(ptrs[i] != NULL);
}

// Free every other block (creates fragmentation)
for (int i = 0; i < 50; i += 2) {
    kfree(ptrs[i]);
    ptrs[i] = NULL;
}

// Try to allocate a large block (should fail due to fragmentation)
void* large = kmalloc(256);  // Needs 2 contiguous blocks
// May fail if coalescing doesn't work

// Cleanup
for (int i = 1; i < 50; i += 2) {
    kfree(ptrs[i]);
}
```
**Expected:** Coalescing should merge free blocks  
**Test:** Measure heap efficiency

#### Test 2.3.4: Sequential Allocation/Free Cycles
```c
for (int cycle = 0; cycle < 10; cycle++) {
    void* ptr = kmalloc(1024);
    assert(ptr != NULL);
    
    // Use memory (prevent optimization)
    memset(ptr, 0xAA, 1024);
    
    kfree(ptr);
}
```
**Expected:** All cycles succeed (memory is recycled)  
**Test:** Verify no memory leaks

#### Test 2.3.5: Random Size Allocations
```c
#define NUM_RANDOM_ALLOCS 200
void* ptrs[NUM_RANDOM_ALLOCS];

for (int i = 0; i < NUM_RANDOM_ALLOCS; i++) {
    size_t size = (rand() % 1024) + 1;  // 1-1024 bytes
    ptrs[i] = kmalloc(size);
    
    if (ptrs[i]) {
        // Write pattern to detect corruption
        memset(ptrs[i], i & 0xFF, size);
    }
}

// Validate no corruption
for (int i = 0; i < NUM_RANDOM_ALLOCS; i++) {
    if (ptrs[i]) {
        size_t size = (rand() % 1024) + 1;  // Recalculate
        uint8_t* p = (uint8_t*)ptrs[i];
        for (size_t j = 0; j < size; j++) {
            if (p[j] != (i & 0xFF)) {
                kprintf("[FAIL] Memory corruption detected at block %d!\n", i);
                break;
            }
        }
    }
}

// Cleanup
for (int i = 0; i < NUM_RANDOM_ALLOCS; i++) {
    if (ptrs[i]) kfree(ptrs[i]);
}
```
**Expected:** No corruption detected  
**Test:** Heap integrity under varied workload

---

### 2.4 Boundary Conditions

#### Test 2.4.1: Block Splitting Edge Case
```c
// Allocate entire heap
void* ptr1 = kmalloc(HEAP_SIZE - sizeof(block_t) - 1024);
assert(ptr1 != NULL);

// Small allocation should split remaining block
void* ptr2 = kmalloc(512);
assert(ptr2 != NULL);  // Should succeed

kfree(ptr1);
kfree(ptr2);
```
**Expected:** Both allocations succeed  
**Test:** Block splitting logic

#### Test 2.4.2: Coalescing Edge Case (Forward)
```c
void* ptr1 = kmalloc(128);
void* ptr2 = kmalloc(128);
void* ptr3 = kmalloc(128);

// Free middle block
kfree(ptr2);

// Free last block (should coalesce with ptr2)
kfree(ptr3);

// Allocate larger block (should fit in coalesced space)
void* ptr4 = kmalloc(256);
assert(ptr4 != NULL);

kfree(ptr1);
kfree(ptr4);
```
**Expected:** Forward coalescing merges ptr2 and ptr3  
**Test:** Forward coalescing logic (line 153-156 in heap.c)

#### Test 2.4.3: Coalescing Edge Case (Backward)
```c
void* ptr1 = kmalloc(128);
void* ptr2 = kmalloc(128);

// Free first block
kfree(ptr1);

// Free second block (should coalesce with ptr1)
kfree(ptr2);

// Allocate larger block
void* ptr3 = kmalloc(256);
assert(ptr3 != NULL);

kfree(ptr3);
```
**Expected:** Backward coalescing merges ptr1 and ptr2  
**Test:** Backward coalescing logic (line 159-174 in heap.c)

#### Test 2.4.4: Heap Exhaustion and Recovery
```c
// Fill heap completely
void* ptrs[1000];
int allocated = 0;

for (int i = 0; i < 1000; i++) {
    ptrs[i] = kmalloc(1024);
    if (ptrs[i]) {
        allocated++;
    } else {
        break;  // Heap full
    }
}

kprintf("[INFO] Allocated %d blocks before OOM\n", allocated);

// Free all
for (int i = 0; i < allocated; i++) {
    kfree(ptrs[i]);
}

// Verify heap is fully recovered
void* test = kmalloc(HEAP_SIZE - sizeof(block_t) - 16);
if (!test) {
    kprintf("[FAIL] Heap not fully recovered after free!\n");
} else {
    kprintf("[PASS] Heap fully recovered\n");
    kfree(test);
}
```
**Expected:** Full recovery after freeing all blocks  
**Test:** Memory leak detection

---

### 2.5 Concurrency Edge Cases

#### Test 2.5.1: Concurrent Allocations (Spinlock Test)
```c
// Not implementable in current single-threaded environment
// Placeholder for future SMP testing
```
**Note:** Current heap uses spinlock (line 17, 82, 146) but system is single-threaded.  
**Future Test:** Multiple cores allocating simultaneously

#### Test 2.5.2: Lock Contention Measurement
```c
// Measure allocation latency variance
uint64_t times[100];
for (int i = 0; i < 100; i++) {
    uint64_t start = rdtsc();
    void* ptr = kmalloc(128);
    uint64_t end = rdtsc();
    times[i] = end - start;
    kfree(ptr);
}

// Calculate statistics
uint64_t min = UINT64_MAX, max = 0, sum = 0;
for (int i = 0; i < 100; i++) {
    if (times[i] < min) min = times[i];
    if (times[i] > max) max = times[i];
    sum += times[i];
}

kprintf("[INFO] Allocation latency: min=%lu, max=%lu, avg=%lu cycles\n",
        (unsigned long)min, (unsigned long)max, (unsigned long)(sum/100));
kprintf("[INFO] Variance: %lux (max/min ratio)\n", (unsigned long)(max/min));
```
**Expected:** Low variance (< 10x)  
**Test:** Lock contention detection (STRESS_TEST_REPORT.md shows 100x variance!)

---

## 3. Safety Mechanism Tests

### 3.1 heap_owns() Validation

#### Test 3.1.1: Valid Heap Pointer
```c
void* ptr = kmalloc(128);
assert(ptr != NULL);

uint64_t addr = (uint64_t)ptr;
assert(addr >= HEAP_START && addr < HEAP_START + HEAP_SIZE);

kfree(ptr);  // Should succeed
```
**Expected:** Free succeeds

#### Test 3.1.2: Stack Pointer Rejection
```c
int stack_var = 42;
uint64_t addr = (uint64_t)&stack_var;

kprintf("[INFO] Stack address: %p (HEAP_START=%p)\n", 
        (void*)addr, (void*)HEAP_START);

kfree(&stack_var);  // Should be rejected silently
```
**Expected:** Silent rejection (line 140-144), no panic

#### Test 3.1.3: Kernel Text Rejection
```c
extern void kernel_main(void);
kfree((void*)kernel_main);  // Should be rejected
```
**Expected:** Silent rejection

#### Test 3.1.4: Below-Heap Pointer
```c
void* below = (void*)(HEAP_START - 1);
kfree(below);  // Should be rejected
```
**Expected:** Silent rejection

#### Test 3.1.5: Above-Heap Pointer
```c
void* above = (void*)(HEAP_START + HEAP_SIZE);
kfree(above);  // Should be rejected
```
**Expected:** Silent rejection

---

### 3.2 Overflow Protection

#### Test 3.2.1: Block Splitting Overflow Check
```c
// Trigger condition on line 95: (uint64_t)current + sizeof(block_t) + size
// This requires a carefully crafted allocation near the end of heap

// Allocate most of heap
size_t large_size = HEAP_SIZE - sizeof(block_t) - 256;
void* ptr1 = kmalloc(large_size);
assert(ptr1 != NULL);

// Allocate small block (should not overflow during split calculation)
void* ptr2 = kmalloc(128);
// If overflow check works, this succeeds or uses whole remaining block

kfree(ptr1);
kfree(ptr2);
```
**Expected:** No overflow, safe behavior  
**Status:** Implemented (line 95-101 in heap.c)

---

### 3.3 Double-Free Detection

#### Test 3.3.1: Immediate Double Free
```c
void* ptr = kmalloc(256);
assert(ptr != NULL);

kfree(ptr);   // First free - OK
kfree(ptr);   // Second free - should panic or be detected
```
**Expected:** Panic with "double free or corruption" message  
**Status:** Implemented via linked list search (line 165-168)

#### Test 3.3.2: Delayed Double Free
```c
void* ptr1 = kmalloc(128);
void* ptr2 = kmalloc(128);

kfree(ptr1);  // Free first
kfree(ptr2);  // Free second (ptr1 might be coalesced)
kfree(ptr1);  // Double-free ptr1
```
**Expected:** Detected via linked list validation  
**Test:** Verify detection survives coalescing

#### Test 3.3.3: Double Free After Reallocation
```c
void* ptr1 = kmalloc(128);
kfree(ptr1);

void* ptr2 = kmalloc(128);  // Might reuse ptr1's memory
kfree(ptr1);  // Double-free (ptr1 might overlap with ptr2)
```
**Expected:** Detection via "block not found in heap" check  
**Risky:** Could cause corruption if ptr2 reused ptr1's location

---

## 4. Performance and Stress Tests

### 4.1 Allocation Performance

#### Test 4.1.1: Throughput Test
```c
#define NUM_ALLOCS 10000
uint64_t start = rdtsc();

for (int i = 0; i < NUM_ALLOCS; i++) {
    void* ptr = kmalloc(128);
    if (ptr) kfree(ptr);
}

uint64_t end = rdtsc();
uint64_t cycles_per_op = (end - start) / (NUM_ALLOCS * 2);

kprintf("[PERF] Alloc+Free: %lu cycles per operation\n", 
        (unsigned long)cycles_per_op);
```
**Baseline:** STRESS_TEST_REPORT.md shows ~1,250 cycles average  
**Goal:** Measure performance degradation under load

#### Test 4.1.2: Fragmentation Performance
```c
// Create fragmented heap
void* ptrs[100];
for (int i = 0; i < 100; i++) {
    ptrs[i] = kmalloc(128);
}
for (int i = 0; i < 100; i += 2) {
    kfree(ptrs[i]);
}

// Measure allocation time with fragmentation
uint64_t start = rdtsc();
void* ptr = kmalloc(128);
uint64_t end = rdtsc();

kprintf("[PERF] Allocation with fragmentation: %lu cycles\n",
        (unsigned long)(end - start));

// Cleanup
if (ptr) kfree(ptr);
for (int i = 1; i < 100; i += 2) {
    kfree(ptrs[i]);
}
```
**Expected:** Higher latency with fragmentation  
**Test:** Quantify fragmentation impact

---

### 4.2 Memory Pressure Tests

#### Test 4.2.1: Gradual OOM Approach
```c
void* ptrs[10000];
int allocated = 0;

for (int i = 0; i < 10000; i++) {
    ptrs[i] = kmalloc(1024);
    if (!ptrs[i]) {
        kprintf("[INFO] OOM at allocation %d (%.2f MB allocated)\n",
                i, (i * 1024.0) / (1024 * 1024));
        break;
    }
    allocated++;
}

kprintf("[INFO] Heap capacity: %d x 1KB blocks = %.2f MB\n",
        allocated, (allocated * 1024.0) / (1024 * 1024));

// Expected: ~15.9 MB (accounting for metadata overhead)

// Cleanup
for (int i = 0; i < allocated; i++) {
    kfree(ptrs[i]);
}
```
**Expected:** ~15,600 allocations (accounting for block_t overhead)  
**Test:** Measure effective heap capacity

#### Test 4.2.2: Repeated OOM Cycles
```c
for (int cycle = 0; cycle < 5; cycle++) {
    void* ptrs[10000];
    int allocated = 0;
    
    // Fill heap
    for (int i = 0; i < 10000; i++) {
        ptrs[i] = kmalloc(512);
        if (ptrs[i]) allocated++;
        else break;
    }
    
    kprintf("[CYCLE %d] Allocated %d blocks\n", cycle, allocated);
    
    // Free all
    for (int i = 0; i < allocated; i++) {
        kfree(ptrs[i]);
    }
}
```
**Expected:** Consistent allocation count across cycles  
**Test:** Detect memory leaks

---

## 5. Test Implementation Structure

### 5.1 Test File: `tests/stress/heap_stress_test.c`

```c
/**
 * Comprehensive Heap Allocator Stress Test
 * 
 * Tests heap.c implementation under extreme conditions:
 * - Edge case allocations (0, huge, overflow)
 * - Fragmentation patterns
 * - Safety mechanism validation
 * - Performance under load
 * - OOM handling
 */

#include "../../kernel/include/kernel.h"
#include "../../kernel/include/mem.h"
#include <stdbool.h>
#include <string.h>

// Test result tracking
typedef struct {
    const char* test_name;
    bool passed;
    const char* failure_reason;
    uint64_t duration_cycles;
} heap_test_result_t;

#define MAX_HEAP_TESTS 50
static heap_test_result_t test_results[MAX_HEAP_TESTS];
static int test_count = 0;

// Test execution macros
#define BEGIN_TEST(name) \
    do { \
        kprintf("\n[TEST] %s\n", name); \
        uint64_t start = rdtsc(); \
        bool test_passed = true; \
        const char* failure_reason = NULL;

#define END_TEST() \
        uint64_t end = rdtsc(); \
        test_results[test_count].test_name = name; \
        test_results[test_count].passed = test_passed; \
        test_results[test_count].failure_reason = failure_reason; \
        test_results[test_count].duration_cycles = end - start; \
        test_count++; \
        if (test_passed) { \
            kprintf("[PASS] %s (duration: %lu cycles)\n", name, \
                    (unsigned long)(end - start)); \
        } else { \
            kprintf("[FAIL] %s: %s\n", name, failure_reason); \
        } \
    } while(0)

#define FAIL_TEST(reason) \
    do { \
        test_passed = false; \
        failure_reason = reason; \
    } while(0)

// Test sections
void test_allocation_edge_cases(void);
void test_deallocation_edge_cases(void);
void test_stress_patterns(void);
void test_boundary_conditions(void);
void test_safety_mechanisms(void);
void test_performance(void);

// Entry point
void heap_stress_test_main(void) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("HEAP ALLOCATOR STRESS TEST SUITE\n");
    kprintf("========================================\n");
    kprintf("Heap: %p - %p (16 MB)\n", 
            (void*)HEAP_START, (void*)(HEAP_START + HEAP_SIZE));
    
    test_allocation_edge_cases();
    test_deallocation_edge_cases();
    test_stress_patterns();
    test_boundary_conditions();
    test_safety_mechanisms();
    test_performance();
    
    // Print summary
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("TEST SUMMARY\n");
    kprintf("========================================\n");
    
    int passed = 0, failed = 0;
    for (int i = 0; i < test_count; i++) {
        if (test_results[i].passed) passed++;
        else failed++;
    }
    
    kprintf("Total: %d tests\n", test_count);
    kprintf("Passed: %d\n", passed);
    kprintf("Failed: %d\n", failed);
    kprintf("Success Rate: %.1f%%\n", (passed * 100.0) / test_count);
    
    if (failed > 0) {
        kprintf("\nFailed Tests:\n");
        for (int i = 0; i < test_count; i++) {
            if (!test_results[i].passed) {
                kprintf("  - %s: %s\n", 
                        test_results[i].test_name,
                        test_results[i].failure_reason);
            }
        }
    }
    
    kprintf("========================================\n");
}
```

### 5.2 Integration with Existing Test Suite

Add to `tests/stress/stress_test_suite.c`:

```c
// Add to main test runner
void run_all_stress_tests(void) {
    // ... existing tests ...
    
    // Add heap stress test
    heap_stress_test_main();
}
```

---

## 6. Expected Outcomes

### 6.1 Success Criteria

All tests should pass with the following behaviors:

| Test | Expected Result |
|------|----------------|
| `kmalloc(0)` | Returns NULL |
| `kmalloc(HEAP_SIZE+1)` | Panics (current) or returns NULL (desired) |
| `kfree(NULL)` | Safe no-op |
| `kfree(stack_ptr)` | Silent rejection via `heap_owns()` |
| Double-free | Panic with "double free" message |
| Small allocations (100x 16 bytes) | All succeed |
| Fragmentation test | Coalescing merges free blocks |
| Heap exhaustion | OOM detected, recovery after free |
| Alignment | All pointers 16-byte aligned |

### 6.2 Known Issues to Document

Based on STRESS_TEST_REPORT.md analysis:

1. **OOM Panic (HIGH)**: `kmalloc` panics instead of returning NULL
   - **Impact**: Legitimate OOM crashes system
   - **Test**: Verify panic behavior, document need for OOM handler

2. **Integer Overflow (MEDIUM)**: No overflow protection in `ALIGN_UP`
   - **Impact**: SIZE_MAX-8 could wrap to 0
   - **Test**: Try `kmalloc((size_t)-8)`

3. **Lock Contention (HIGH)**: Single global lock causes 100x variance
   - **Impact**: Performance degradation under concurrent load
   - **Test**: Measure allocation latency variance

4. **No Magic Numbers (CRITICAL)**: Blocks lack integrity markers
   - **Impact**: Corruption detection is weak
   - **Test**: Document vulnerability

5. **No Canaries (CRITICAL)**: Buffer overflows undetected
   - **Impact**: Silent heap corruption
   - **Test**: Document vulnerability (can't test without implementation)

---

## 7. Test Execution Plan

### Phase 1: Basic Validation (Priority 1)
1. Test 2.1.1 - Zero-size allocation
2. Test 2.2.1 - NULL pointer free
3. Test 2.1.4 - Single byte allocation (alignment)
4. Test 2.3.1 - Many small allocations (100x 16 bytes)
5. Test 3.1.2 - Stack pointer rejection

**Goal:** Validate basic safety mechanisms work

### Phase 2: Edge Cases (Priority 2)
1. Test 2.1.2 - Massive allocation (OOM)
2. Test 2.2.2 - Double-free detection
3. Test 2.3.3 - Fragmentation pattern
4. Test 2.4.4 - Heap exhaustion and recovery

**Goal:** Identify breaking points

### Phase 3: Stress Patterns (Priority 3)
1. Test 2.3.5 - Random size allocations
2. Test 4.2.1 - Gradual OOM approach
3. Test 4.2.2 - Repeated OOM cycles
4. Test 4.1.2 - Fragmentation performance

**Goal:** Measure heap efficiency under realistic load

### Phase 4: Performance (Priority 4)
1. Test 4.1.1 - Throughput test
2. Test 2.5.2 - Lock contention measurement

**Goal:** Establish performance baseline

---

## 8. Bug Documentation Template

For each bug found, document:

```markdown
### BUG-XXX: [Short Description]

**Severity:** CRITICAL / HIGH / MEDIUM / LOW  
**Location:** `kernel/core/mem/heap.c:[line]`  
**Discovered By:** Heap stress test [test_name]

**Issue:**
[Detailed description]

**Reproduction:**
[Code snippet]

**Expected Behavior:**
[What should happen]

**Actual Behavior:**
[What currently happens]

**Fix Required:**
[Proposed solution]

**Security Impact:**
[Potential exploit scenario]
```

---

## 9. References

- **Heap Implementation**: `C:\Users\wilde\Desktop\Kernel\kernel\core\mem\heap.c`
- **Memory Header**: `C:\Users\wilde\Desktop\Kernel\kernel\include\mem.h`
- **Existing Unit Test**: `C:\Users\wilde\Desktop\Kernel\tests\unit\test_heap.c`
- **Heap Fuzzer**: `C:\Users\wilde\Desktop\Kernel\tests\fuzz\heap_fuzzer.c`
- **Stress Test Report**: `C:\Users\wilde\Desktop\Kernel\tests\stress\STRESS_TEST_REPORT.md`
- **Existing Stress Suite**: `C:\Users\wilde\Desktop\Kernel\tests\stress\stress_test_suite.c`

---

## 10. Appendix: Quick Reference

### Heap Constants
```c
HEAP_START = 0xFFFFFFFF90000000ULL
HEAP_SIZE  = 16 * 1024 * 1024  // 16 MB
ALIGNMENT  = 16 bytes
```

### Block Structure Size
```c
sizeof(block_t) = sizeof(size_t) + sizeof(bool) + sizeof(void*)
                = 8 + 1 + 8 = 17 bytes (likely padded to 24 bytes)
```

### Effective Heap Capacity
```text
Total: 16 MB = 16,777,216 bytes
Minus initial metadata: 16,777,216 - 24 = 16,777,192 bytes usable
Per-block overhead: 24 bytes

For 1KB allocations: 16,777,192 / (1024 + 24) ≈ 16,007 allocations
For 16B allocations: 16,777,192 / (16 + 24) ≈ 419,429 allocations
```

### OOM Calculation
```text
Minimum allocation (after alignment): 16 bytes
With overhead: 16 + 24 = 40 bytes per allocation
Maximum small allocations: 16,777,216 / 40 ≈ 419,430
```

---

**Document Status:** DRAFT  
**Author:** AI Analysis (Claude Sonnet 4.5)  
**Date:** 2026-05-26  
**Next Steps:** Implement test suite in `tests/stress/heap_stress_test.c`
