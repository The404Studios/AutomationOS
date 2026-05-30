# Edge Case Testing Guide for AutomationOS

## Mission
**Find and fix bugs in edge cases that normal testing misses.**

## Overview

Edge cases are boundary conditions that occur rarely but can cause catastrophic failures. This guide documents all edge cases tested in AutomationOS and provides a systematic approach to edge case testing.

## Edge Case Categories

### 1. Boundary Values
Edge cases at the limits of acceptable ranges.

#### Zero (0)
- `kmalloc(0)` → Should return NULL
- `copy_from_user(..., 0)` → Should return EFAULT
- Zero-length buffers in all syscalls
- Zero timeout values

**Test Coverage:**
- ✅ `test_zero_allocation()`
- ✅ `test_copy_from_user_zero_size()`

#### One (1)
- `kmalloc(1)` → Should succeed with minimum allocation
- Single byte reads/writes
- Single page allocations

**Test Coverage:**
- ✅ `test_one_byte_allocation()`

#### Maximum Values
- `kmalloc(UINT32_MAX)` → Should fail gracefully
- `kmalloc(UINT64_MAX)` → Should fail gracefully
- Maximum buffer sizes in syscalls
- Maximum number of file descriptors
- Maximum PID value

**Test Coverage:**
- ✅ `test_max_allocation()`
- ✅ `test_syscall_boundary_values()`

#### Negative Values (-1)
- `sys_write(-1, ...)` → Should return EBADF
- Negative sizes (wrapped to huge positive)
- Negative PIDs

**Test Coverage:**
- ✅ `test_negative_values()`

#### NULL Pointers
- `kfree(NULL)` → Should be safe no-op
- `process_ref(NULL)` → Should be safe no-op
- `validate_user_buffer(NULL, ...)` → Should return false
- All functions should handle NULL gracefully

**Test Coverage:**
- ✅ `test_null_free()`
- ✅ `test_process_ref_null()`
- ✅ `test_invalid_pointers()`

### 2. Resource Exhaustion
Testing behavior when resources run out.

#### Out of Memory
- Heap completely full
- All physical pages allocated
- Cannot allocate more processes

**Behavior:**
- System should remain stable
- Allocation failures should return NULL/error codes
- No crashes or undefined behavior
- Graceful degradation

**Test Coverage:**
- ✅ `test_heap_complete_exhaustion()`
- ✅ `test_physical_memory_exhaustion()`
- ✅ `test_out_of_memory_handling()`

#### Out of File Descriptors
- All FDs allocated
- Cannot open more files

**Test Coverage:**
- ⏳ `test_file_descriptor_exhaustion()` (TODO: requires filesystem)

#### Out of PIDs
- Process table full
- Cannot create more processes

**Test Coverage:**
- ✅ `test_process_table_exhaustion()`

#### Out of Disk Space
- Write fails when disk full
- Filesystem remains consistent

**Test Coverage:**
- ⏳ TODO (requires filesystem)

### 3. Concurrent Access (Race Conditions)
Testing thread safety and atomicity.

#### Two Threads Allocate Last Page
```c
// Only one page remains
void* page1 = pmm_alloc_page();  // Thread 1
void* page2 = pmm_alloc_page();  // Thread 2
// At most ONE should succeed
```

**Expected:** Spinlock ensures atomic access; exactly 0 or 1 succeeds.

**Test Coverage:**
- ✅ `test_last_page_race()`

#### Two Processes Create Same File
```c
int fd1 = open("file", O_CREAT | O_EXCL);  // Process 1
int fd2 = open("file", O_CREAT | O_EXCL);  // Process 2
// Exactly ONE should succeed
```

**Expected:** Atomic file creation ensures no race.

**Test Coverage:**
- ⏳ TODO (requires filesystem)

#### Race to Acquire Last Resource
- Last FD
- Last PID
- Last memory block

**Test Coverage:**
- ✅ `test_last_page_race()`
- ✅ `test_process_creation_race()`

#### Reference Count Races
```c
process_ref(proc);   // Thread 1
process_unref(proc); // Thread 2
// Reference count must be atomic
```

**Test Coverage:**
- ✅ `test_reference_count_race()`

#### Double Free Race
```c
kfree(ptr);  // Thread 1
kfree(ptr);  // Thread 2
// Must detect and prevent
```

**Test Coverage:**
- ✅ `test_double_free_race()`

### 4. Invalid Input
Testing input validation and sanitization.

#### Buffer Overflow
```c
char buf[16];
copy_from_user(buf, user_ptr, 0xFFFFFFFF);  // Overflow!
```

**Expected:** Size validation prevents overflow.

**Test Coverage:**
- ✅ `test_buffer_overflow_protection()`

#### Integer Overflow
```c
void* ptr = (void*)0xFFFFFFFFFFFFFFF0;
validate_user_buffer(ptr, 0x100);  // ptr + size overflows!
```

**Expected:** Overflow detection in validation.

**Test Coverage:**
- ✅ `test_integer_overflow()`

#### Path Traversal
```c
open("../../etc/passwd");
open("..\\..\\windows\\system32");
```

**Expected:** Path validation rejects traversal.

**Test Coverage:**
- ✅ `test_path_traversal()` (pattern identification)

#### Invalid Pointers
- NULL pointers
- Kernel space pointers (when expecting user space)
- Unaligned pointers
- Random garbage addresses

**Test Coverage:**
- ✅ `test_invalid_pointers()`

#### Malformed Data Structures
- Corrupted process state
- Invalid enum values
- Circular linked lists
- Dangling pointers

**Test Coverage:**
- ✅ `test_malformed_data()`

### 5. Timing Issues
Testing time-dependent behavior.

#### Very Fast Operations
- Rapid alloc/free cycles
- Rapid process creation/destruction
- High-frequency syscalls

**Behavior:** Should handle without corruption.

**Test Coverage:**
- ✅ `test_rapid_operations()`
- ✅ `test_high_frequency_alloc_free()`

#### Very Slow Operations
- Long-running syscalls
- Slow I/O operations

**Test Coverage:**
- ⏳ TODO

#### Timeout Edge Cases
- Zero timeout
- Maximum timeout
- Timeout exactly when operation completes

**Test Coverage:**
- ✅ `test_zero_timeout()` (partial)
- ✅ `test_max_timeout()` (partial)

#### Interrupt During Critical Section
```c
spin_lock(&lock);
// What if interrupt occurs here?
// Critical section
spin_unlock(&lock);
```

**Expected:** Spinlocks disable interrupts.

**Test Coverage:**
- ✅ `test_interrupt_safety()`

### 6. Edge Case Combinations
Multiple edge cases occurring simultaneously.

#### Use-After-Free
```c
void* ptr = kmalloc(64);
kfree(ptr);
*(char*)ptr = 'X';  // Use after free!
```

**Expected:** Memory protection catches this.

**Test Coverage:**
- ✅ `test_null_after_free()`

#### Double Free
```c
kfree(ptr);
kfree(ptr);  // Double free!
```

**Expected:** Heap corruption detection.

**Test Coverage:**
- ✅ `test_double_free()`

#### Alignment Issues
```c
kmalloc(1);   // Odd size
kmalloc(3);   // Odd size
kmalloc(127); // Odd size
```

**Expected:** All allocations properly aligned.

**Test Coverage:**
- ✅ `test_alignment_edge_cases()`

#### Memory Exhaustion During OOM Handler
- OOM handler itself allocates memory
- What if that allocation fails?

**Expected:** OOM handler uses pre-reserved memory.

**Test Coverage:**
- ✅ `test_nested_oom()`

#### Fragmentation
- Allocate many blocks
- Free every other block
- Try to allocate large block

**Expected:** Coalescing or graceful failure.

**Test Coverage:**
- ✅ `test_worst_case_fragmentation()`

## Test Suites

### 1. test_edge_cases.c
Comprehensive edge case testing covering:
- Boundary values (0, 1, MAX, -1, NULL)
- Resource exhaustion
- Concurrent access
- Invalid input
- Timing issues
- Edge case combinations

**Run:** `make test-edge-cases`

### 2. test_race_conditions.c
Race condition and concurrency testing:
- Race for last page
- Double free race
- Reference count race
- Lock ordering
- Memory ordering
- Interrupt safety

**Run:** `make test-race-conditions`

### 3. test_resource_exhaustion.c
Resource exhaustion testing:
- Complete memory exhaustion
- Process table full
- Fragmentation
- Memory leaks
- Cascading failures

**Run:** `make test-resource-exhaustion`

## Testing Strategy

### For Each Subsystem

When implementing or modifying a subsystem, test these edge cases:

#### Memory Management
```c
// Zero allocation
void* ptr = kmalloc(0);
assert(ptr == NULL);

// Maximum allocation
ptr = kmalloc(UINT32_MAX);
assert(ptr == NULL);

// NULL free
kfree(NULL);  // Should not crash

// Double free detection
ptr = kmalloc(64);
kfree(ptr);
// kfree(ptr);  // Should panic or be detected

// Exhaustion
while ((ptr = kmalloc(1024)) != NULL);
// System should still work
```

#### Process Management
```c
// NULL checks
process_ref(NULL);    // Should handle safely
process_unref(NULL);  // Should handle safely

// Process table exhaustion
while (process_create(...) != NULL);
// Should fail gracefully

// Rapid creation/destruction
for (int i = 0; i < 10000; i++) {
    process_t* p = process_create(...);
    process_destroy(p);
}
```

#### System Calls
```c
// Invalid syscall number
syscall_dispatch(MAX_SYSCALLS + 1, ...);  // Should return EINVAL

// Negative file descriptor
sys_write(-1, buf, len);  // Should return EBADF

// NULL buffer
sys_read(0, NULL, 100);  // Should return EFAULT

// Huge size
sys_write(1, buf, UINT64_MAX);  // Should return EINVAL
```

## Common Edge Case Bugs

### CWE-476: NULL Pointer Dereference
```c
// BAD
void process_destroy(process_t* proc) {
    kfree(proc->kernel_stack);  // What if proc is NULL?
}

// GOOD
void process_destroy(process_t* proc) {
    if (!proc) return;  // NULL check
    kfree(proc->kernel_stack);
}
```

### CWE-190: Integer Overflow
```c
// BAD
bool validate_buffer(void* ptr, size_t size) {
    return (ptr + size < USER_SPACE_END);  // Overflow!
}

// GOOD
bool validate_buffer(void* ptr, size_t size) {
    uint64_t end = (uint64_t)ptr + size;
    if (end < (uint64_t)ptr) return false;  // Overflow check
    return (end <= USER_SPACE_END);
}
```

### CWE-415: Double Free
```c
// BAD
void kfree(void* ptr) {
    block_t* block = get_block(ptr);
    block->is_free = true;  // No check if already free!
}

// GOOD
void kfree(void* ptr) {
    if (!ptr) return;
    block_t* block = get_block(ptr);
    if (block->is_free) {
        kernel_panic("Double free detected");
    }
    block->is_free = true;
}
```

### CWE-362: Race Condition
```c
// BAD
void* pmm_alloc_page(void) {
    if (free_list != NULL) {
        page_t* page = free_list;  // Race here!
        free_list = page->next;    // Another thread could allocate same page
        return page;
    }
}

// GOOD
void* pmm_alloc_page(void) {
    spin_lock(&pmm_lock);
    if (free_list != NULL) {
        page_t* page = free_list;
        free_list = page->next;
        spin_unlock(&pmm_lock);
        return page;
    }
    spin_unlock(&pmm_lock);
    return NULL;
}
```

### CWE-401: Memory Leak
```c
// BAD
process_t* process_create(...) {
    process_t* proc = kmalloc(sizeof(process_t));
    proc->kernel_stack = pmm_alloc_page();
    if (!proc->kernel_stack) {
        return NULL;  // Leaked proc!
    }
    return proc;
}

// GOOD
process_t* process_create(...) {
    process_t* proc = kmalloc(sizeof(process_t));
    proc->kernel_stack = pmm_alloc_page();
    if (!proc->kernel_stack) {
        kfree(proc);  // Clean up on failure
        return NULL;
    }
    return proc;
}
```

## Running the Tests

### Build and Run All Edge Case Tests
```bash
make test-edge-cases
```

### Run Individual Test Suites
```bash
# Boundary value and invalid input tests
make test-edge-cases-unit

# Race condition tests
make test-race-conditions

# Resource exhaustion tests
make test-resource-exhaustion
```

### Quick Test
```bash
./test_edge_cases.sh
```

## Expected Output

```
================================================
  AutomationOS Edge Case Test Suite
  Comprehensive Edge Case Testing
================================================

=== CATEGORY 1: BOUNDARY VALUES ===
[TEST] kmalloc(0) should return NULL
  PASS: kmalloc(0) correctly returned NULL

[TEST] kmalloc(1) should succeed
  PASS: kmalloc(1) works correctly

...

========================================
  All Edge Case Tests Passed
  Memory Safety: VERIFIED
========================================
```

## Adding New Edge Case Tests

### 1. Identify the Edge Case
- What is the boundary condition?
- What is the expected behavior?
- What could go wrong?

### 2. Write the Test
```c
void test_new_edge_case(void) {
    TEST("New edge case description");

    // Setup
    void* resource = allocate_resource();

    // Trigger edge case
    int result = edge_case_operation(resource);

    // Verify expected behavior
    EXPECT(result == EXPECTED_VALUE, "Edge case not handled");

    // Cleanup
    free_resource(resource);

    kprintf("  PASS: New edge case handled\n");
}
```

### 3. Add to Test Suite
```c
void run_edge_case_tests(void) {
    // ... existing tests ...
    test_new_edge_case();
}
```

### 4. Document It
Add entry to this guide explaining:
- The edge case
- Expected behavior
- Test coverage
- Related CWE if applicable

## Checklist: Edge Case Coverage

For each new feature, verify:

- [ ] **Zero values tested** (0, empty, null)
- [ ] **Boundary values tested** (1, MAX-1, MAX)
- [ ] **Negative values tested** (-1, wrapped values)
- [ ] **NULL pointers handled** (all pointer parameters)
- [ ] **Resource exhaustion handled** (OOM, table full)
- [ ] **Race conditions prevented** (atomic operations, locks)
- [ ] **Integer overflow checked** (address + size)
- [ ] **Buffer overflow prevented** (size validation)
- [ ] **Invalid input rejected** (bad pointers, bad values)
- [ ] **Memory leaks prevented** (cleanup on error)
- [ ] **Double free prevented** (free state checking)
- [ ] **Use-after-free prevented** (reference counting)

## Statistics

### Edge Cases Tested
- Boundary values: 15 tests
- Resource exhaustion: 12 tests
- Race conditions: 14 tests
- Invalid input: 8 tests
- Timing issues: 5 tests
- Combinations: 7 tests

**Total: 61 edge case tests**

### CWE Coverage
- CWE-476 (NULL Pointer Dereference): ✅ Covered
- CWE-190 (Integer Overflow): ✅ Covered
- CWE-119 (Buffer Overflow): ✅ Covered
- CWE-415 (Double Free): ✅ Covered
- CWE-416 (Use After Free): ✅ Covered
- CWE-401 (Memory Leak): ✅ Covered
- CWE-362 (Race Condition): ✅ Covered
- CWE-755 (Improper Error Handling): ✅ Covered

## References

### Standards
- CERT C Coding Standard
- MISRA C Guidelines
- CWE Top 25

### Related Documents
- `tests/unit/test_edge_cases.c` - Main test suite
- `tests/unit/test_race_conditions.c` - Concurrency tests
- `tests/unit/test_resource_exhaustion.c` - Exhaustion tests
- `kernel/include/mem.h` - Memory management API
- `kernel/include/sched.h` - Process management API

## Continuous Improvement

This guide and test suite should be updated when:
1. New edge cases are discovered
2. New subsystems are added
3. Edge case bugs are found
4. New vulnerability classes emerge

**Remember:** Edge cases are where bugs hide. Test early, test often, test thoroughly.
