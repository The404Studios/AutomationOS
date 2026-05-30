# Test Best Practices for AutomationOS

## Table of Contents

1. [General Principles](#general-principles)
2. [Test Structure](#test-structure)
3. [Naming Conventions](#naming-conventions)
4. [Assertion Guidelines](#assertion-guidelines)
5. [Test Coverage](#test-coverage)
6. [Performance Testing](#performance-testing)
7. [Code Review Checklist](#code-review-checklist)
8. [Common Pitfalls](#common-pitfalls)

## General Principles

### The Three A's: Arrange, Act, Assert

```c
KTEST_CASE(pmm, alloc_returns_aligned_pages) {
    // Arrange - Set up test data
    pmm_init(test_mmap, test_mmap_count);

    // Act - Perform the operation
    void* page = pmm_alloc_page();

    // Assert - Verify results
    KTEST_ASSERT_NOT_NULL(page);
    KTEST_ASSERT_EQ((uintptr_t)page % PAGE_SIZE, 0);

    // Cleanup
    pmm_free_page(page);
}
```

### FIRST Principles

- **F**ast: Tests should run quickly (< 100ms per test)
- **I**solated: Tests should not depend on each other
- **R**epeatable: Same results every time
- **S**elf-validating: Pass/fail, no manual inspection
- **T**imely: Written alongside code, not after

### Test-Driven Development (TDD)

1. **Red**: Write a failing test
2. **Green**: Write minimal code to pass
3. **Refactor**: Improve code while keeping tests green

```c
// Step 1: Write failing test
KTEST_CASE(heap, calloc_zeros_memory) {
    uint32_t* ptr = (uint32_t*)kcalloc(64, sizeof(uint32_t));
    KTEST_ASSERT_NOT_NULL(ptr);

    for (int i = 0; i < 64; i++) {
        KTEST_ASSERT_EQ(ptr[i], 0);  // Will fail initially
    }

    kfree(ptr);
}

// Step 2: Implement kcalloc() to pass test
// Step 3: Refactor implementation
```

## Test Structure

### Use Fixtures for Complex Setup

```c
// Good - Reusable fixture
typedef struct {
    file_descriptor_t* fds[10];
    char* temp_files[10];
    int fd_count;
} fs_fixture_t;

static void fs_setup(fs_fixture_t* f) {
    f->fd_count = 0;
    for (int i = 0; i < 10; i++) {
        f->fds[i] = NULL;
        f->temp_files[i] = NULL;
    }
}

static void fs_teardown(fs_fixture_t* f) {
    for (int i = 0; i < f->fd_count; i++) {
        if (f->fds[i]) close_fd(f->fds[i]);
        if (f->temp_files[i]) delete_file(f->temp_files[i]);
    }
}

KTEST_SUITE_WITH_FIXTURE(fs, fs_fixture_t, fs_setup, fs_teardown);
```

### Keep Tests Small and Focused

```c
// Bad - Tests multiple things
KTEST_CASE(pmm, test_everything) {
    void* p1 = pmm_alloc_page();
    KTEST_ASSERT_NOT_NULL(p1);
    KTEST_ASSERT_EQ((uintptr_t)p1 % PAGE_SIZE, 0);

    void* p2 = pmm_alloc_page();
    KTEST_ASSERT_NE(p1, p2);

    pmm_free_page(p1);
    uint64_t used = pmm_get_used_memory();
    KTEST_ASSERT_GT(used, 0);

    // Too many unrelated assertions!
}

// Good - Focused tests
KTEST_CASE(pmm, alloc_returns_non_null) {
    void* page = pmm_alloc_page();
    KTEST_ASSERT_NOT_NULL(page);
    pmm_free_page(page);
}

KTEST_CASE(pmm, alloc_returns_aligned_pages) {
    void* page = pmm_alloc_page();
    KTEST_ASSERT_EQ((uintptr_t)page % PAGE_SIZE, 0);
    pmm_free_page(page);
}

KTEST_CASE(pmm, alloc_returns_different_pages) {
    void* p1 = pmm_alloc_page();
    void* p2 = pmm_alloc_page();
    KTEST_ASSERT_NE(p1, p2);
    pmm_free_page(p1);
    pmm_free_page(p2);
}
```

### Test One Logical Concept per Test

```c
// Good - Single concept: boundary checking
KTEST_CASE(vmm, map_respects_address_boundaries) {
    void* page = pmm_alloc_page();

    // Test lower boundary
    int result = vmm_map_page(0x0, (uintptr_t)page, VM_READ);
    KTEST_ASSERT_NE(result, 0);  // Should fail

    // Test upper boundary
    result = vmm_map_page(0xFFFFFFFFFFFFFFFF, (uintptr_t)page, VM_READ);
    KTEST_ASSERT_NE(result, 0);  // Should fail

    pmm_free_page(page);
}
```

## Naming Conventions

### Test Suite Names

```c
// Good - Clear subsystem names
KTEST_SUITE(pmm);           // Physical memory manager
KTEST_SUITE(vmm);           // Virtual memory manager
KTEST_SUITE(sched);         // Scheduler
KTEST_SUITE(syscall);       // System calls
KTEST_SUITE(fs_ext4);       // ext4 filesystem

// Bad - Unclear names
KTEST_SUITE(test);
KTEST_SUITE(my_tests);
KTEST_SUITE(suite1);
```

### Test Case Names

Use pattern: `<action>_<expected_result>` or `<state>_<action>_<result>`

```c
// Good - Descriptive names
KTEST_CASE(heap, malloc_returns_non_null)
KTEST_CASE(heap, free_null_is_safe)
KTEST_CASE(heap, realloc_preserves_data)
KTEST_CASE(heap, calloc_zeros_memory)
KTEST_CASE(sched, create_process_returns_unique_pid)
KTEST_CASE(sched, empty_queue_returns_idle)
KTEST_CASE(syscall, read_with_null_buffer_returns_error)

// Bad - Unclear names
KTEST_CASE(heap, test1)
KTEST_CASE(heap, test_malloc)
KTEST_CASE(heap, check_stuff)
KTEST_CASE(sched, func_test)
```

### Fixture Names

```c
// Good
typedef struct {
    void* buffer;
    size_t size;
} heap_fixture_t;

typedef struct {
    process_t* processes[10];
    int process_count;
} sched_fixture_t;

// Bad
typedef struct {
    void* data;
} test_data_t;

typedef struct {
    int x, y, z;
} fixture_t;
```

## Assertion Guidelines

### Choose the Right Assertion

```c
// Good - Specific assertions
KTEST_ASSERT_NULL(ptr);              // For NULL checks
KTEST_ASSERT_NOT_NULL(ptr);          // For non-NULL checks
KTEST_ASSERT_EQ(a, b);               // For equality
KTEST_ASSERT_PTR_EQ(p1, p2);         // For pointer equality
KTEST_ASSERT_STR_EQ(s1, s2);         // For string equality

// Bad - Generic assertions
KTEST_ASSERT(ptr == NULL);           // Use KTEST_ASSERT_NULL
KTEST_ASSERT(a == b);                // Use KTEST_ASSERT_EQ
KTEST_ASSERT(!strcmp(s1, s2));       // Use KTEST_ASSERT_STR_EQ
```

### Add Meaningful Messages

```c
// Good - Clear error context
KTEST_ASSERT_MSG(page_count > 0,
    "No pages available in free list");

KTEST_ASSERT_MSG(addr % PAGE_SIZE == 0,
    "Address 0x%lx is not page-aligned", addr);

// Bad - No context
KTEST_ASSERT(page_count > 0);
KTEST_ASSERT(addr % PAGE_SIZE == 0);
```

### Assert Preconditions First

```c
// Good - Check preconditions
KTEST_CASE(vmm, map_page_succeeds) {
    void* phys = pmm_alloc_page();
    KTEST_ASSERT_NOT_NULL(phys);  // Precondition

    int result = vmm_map_page(0x10000000, (uintptr_t)phys, VM_READ);
    KTEST_ASSERT_EQ(result, 0);   // Actual test

    vmm_unmap_page(0x10000000);
    pmm_free_page(phys);
}
```

### Use EXPECT for Multiple Assertions

```c
// Good - All assertions run
KTEST_CASE(process, all_fields_initialized) {
    process_t* proc = process_create("test", 1);
    KTEST_ASSERT_NOT_NULL(proc);  // Fatal if NULL

    // All of these will run even if some fail
    KTEST_EXPECT_NOT_NULL(proc->name);
    KTEST_EXPECT_EQ(proc->state, PROCESS_READY);
    KTEST_EXPECT_GT(proc->pid, 0);
    KTEST_EXPECT_EQ(proc->priority, 1);

    process_destroy(proc);
}
```

## Test Coverage

### What to Test

#### 1. Happy Path

```c
KTEST_CASE(heap, malloc_basic_usage) {
    void* ptr = kmalloc(100);
    KTEST_ASSERT_NOT_NULL(ptr);

    // Use memory
    memset(ptr, 0xAA, 100);

    kfree(ptr);
}
```

#### 2. Edge Cases

```c
KTEST_CASE(heap, malloc_zero_size) {
    void* ptr = kmalloc(0);
    // Implementation-defined behavior
    if (ptr) kfree(ptr);
}

KTEST_CASE(heap, malloc_max_size) {
    void* ptr = kmalloc(SIZE_MAX);
    KTEST_ASSERT_NULL(ptr);  // Should fail
}
```

#### 3. Boundary Conditions

```c
KTEST_CASE(array, access_first_element) {
    int arr[10];
    arr[0] = 42;
    KTEST_ASSERT_EQ(arr[0], 42);
}

KTEST_CASE(array, access_last_element) {
    int arr[10];
    arr[9] = 42;
    KTEST_ASSERT_EQ(arr[9], 42);
}
```

#### 4. Error Handling

```c
KTEST_CASE(syscall, read_with_invalid_fd) {
    char buffer[100];
    ssize_t result = sys_read(-1, buffer, 100);
    KTEST_ASSERT_EQ(result, -EBADF);
}

KTEST_CASE(syscall, write_with_null_buffer) {
    ssize_t result = sys_write(1, NULL, 100);
    KTEST_ASSERT_EQ(result, -EFAULT);
}
```

#### 5. Resource Cleanup

```c
KTEST_CASE(pmm, free_actually_frees_memory) {
    uint64_t before = pmm_get_used_memory();

    void* page = pmm_alloc_page();
    uint64_t after_alloc = pmm_get_used_memory();
    KTEST_ASSERT_EQ(after_alloc, before + PAGE_SIZE);

    pmm_free_page(page);
    uint64_t after_free = pmm_get_used_memory();
    KTEST_ASSERT_EQ(after_free, before);
}
```

#### 6. Concurrent Access (if applicable)

```c
KTEST_CASE(sched, multiple_threads_same_resource) {
    // Test thread safety
    spinlock_t lock;
    spinlock_init(&lock);

    // Simulate concurrent access
    for (int i = 0; i < 100; i++) {
        spinlock_acquire(&lock);
        shared_resource++;
        spinlock_release(&lock);
    }

    KTEST_ASSERT_EQ(shared_resource, 100);
}
```

### Coverage Goals

- **Line Coverage**: Aim for 80%+ on critical paths
- **Branch Coverage**: Test both true and false branches
- **Path Coverage**: Test different execution paths
- **Function Coverage**: Test all public APIs

## Performance Testing

### Basic Benchmarking

```c
KTEST_CASE(heap, malloc_performance) {
    ktest_benchmark_start();

    for (int i = 0; i < 1000; i++) {
        void* ptr = kmalloc(64);
        kfree(ptr);
    }

    uint64_t cycles = ktest_benchmark_end("1000 malloc/free cycles");

    // Assert reasonable performance
    KTEST_ASSERT_LT(cycles, 10000000);  // Less than 10M cycles
}
```

### Stress Testing

```c
KTEST_CASE(pmm, stress_test_allocation) {
    #define STRESS_ITERATIONS 1000
    void* pages[STRESS_ITERATIONS];

    // Allocate many pages
    for (int i = 0; i < STRESS_ITERATIONS; i++) {
        pages[i] = pmm_alloc_page();
        KTEST_ASSERT_NOT_NULL(pages[i]);
    }

    // Free all pages
    for (int i = 0; i < STRESS_ITERATIONS; i++) {
        pmm_free_page(pages[i]);
    }

    // Memory should be back to normal
    uint64_t used = pmm_get_used_memory();
    KTEST_ASSERT_LT(used, PAGE_SIZE * 10);  // Minimal overhead
}
```

### Performance Regression Detection

```c
KTEST_CASE(syscall, getpid_performance_baseline) {
    ktest_benchmark_start();

    for (int i = 0; i < 10000; i++) {
        sys_getpid();
    }

    uint64_t cycles = ktest_benchmark_end("10000 getpid calls");

    // Baseline: 5000 cycles (measured empirically)
    // Allow 20% variance
    KTEST_ASSERT_LT(cycles, 6000);
    KTEST_ASSERT_GT(cycles, 4000);
}
```

## Code Review Checklist

When reviewing test code, check:

### Structure
- [ ] Test name clearly describes what is being tested
- [ ] Test follows Arrange-Act-Assert pattern
- [ ] Test is focused on one logical concept
- [ ] Fixtures are used appropriately
- [ ] Resources are properly cleaned up

### Assertions
- [ ] Appropriate assertion types are used
- [ ] Preconditions are checked first
- [ ] Error messages provide context
- [ ] Expected value comes before actual in assertions

### Coverage
- [ ] Happy path is tested
- [ ] Error cases are tested
- [ ] Boundary conditions are tested
- [ ] Edge cases are considered

### Quality
- [ ] No hardcoded magic numbers
- [ ] No test-specific code in production
- [ ] Tests don't depend on execution order
- [ ] Tests run quickly (< 100ms)

### Documentation
- [ ] Complex test logic is commented
- [ ] Non-obvious setup is explained
- [ ] References to bugs/tickets are included

## Common Pitfalls

### 1. Test Interdependence

```c
// Bad - Tests depend on each other
static int global_counter = 0;

KTEST_CASE(bad, test_one) {
    global_counter = 5;
}

KTEST_CASE(bad, test_two) {
    KTEST_ASSERT_EQ(global_counter, 5);  // Fails if test_one doesn't run
}

// Good - Independent tests
KTEST_CASE(good, test_one) {
    int counter = 5;
    KTEST_ASSERT_EQ(counter, 5);
}

KTEST_CASE(good, test_two) {
    int counter = 0;
    counter += 5;
    KTEST_ASSERT_EQ(counter, 5);
}
```

### 2. Testing Implementation Instead of Behavior

```c
// Bad - Tests internal implementation
KTEST_CASE(bad, heap_uses_free_list) {
    heap_t* heap = get_heap();
    KTEST_ASSERT_NOT_NULL(heap->free_list);  // Internal detail
}

// Good - Tests observable behavior
KTEST_CASE(good, heap_reuses_freed_memory) {
    void* ptr1 = kmalloc(64);
    uintptr_t addr1 = (uintptr_t)ptr1;
    kfree(ptr1);

    void* ptr2 = kmalloc(64);
    uintptr_t addr2 = (uintptr_t)ptr2;

    // Should reuse same memory
    KTEST_ASSERT_EQ(addr1, addr2);
    kfree(ptr2);
}
```

### 3. Resource Leaks in Tests

```c
// Bad - Leaks memory
KTEST_CASE(bad, test_with_leak) {
    void* ptr = kmalloc(100);
    KTEST_ASSERT_NOT_NULL(ptr);
    // Missing kfree(ptr)!
}

// Good - Proper cleanup
KTEST_CASE(good, test_with_cleanup) {
    void* ptr = kmalloc(100);
    KTEST_ASSERT_NOT_NULL(ptr);
    kfree(ptr);
}

// Better - Use fixtures
static void setup(test_fixture_t* f) {
    f->ptr = kmalloc(100);
}

static void teardown(test_fixture_t* f) {
    kfree(f->ptr);  // Automatic cleanup
}
```

### 4. Flaky Tests

```c
// Bad - Timing-dependent test
KTEST_CASE(bad, flaky_timing_test) {
    start_async_operation();
    sleep_ms(100);  // Race condition!
    KTEST_ASSERT_TRUE(operation_complete());
}

// Good - Wait for condition
KTEST_CASE(good, reliable_timing_test) {
    start_async_operation();

    // Wait with timeout
    for (int i = 0; i < 1000; i++) {
        if (operation_complete()) break;
        sleep_ms(1);
    }

    KTEST_ASSERT_TRUE(operation_complete());
}
```

### 5. Over-Mocking

```c
// Bad - Too much mocking
KTEST_CASE(bad, over_mocked) {
    mock_pmm_alloc();
    mock_vmm_map();
    mock_heap_init();
    mock_process_create();
    // Now testing mocks, not real code!
}

// Good - Mock only external dependencies
KTEST_CASE(good, appropriate_mocking) {
    mock_disk_read();  // External dependency
    // Test real kernel code that uses disk
    int result = load_file("test.txt");
    KTEST_ASSERT_EQ(result, 0);
}
```

## Summary

Follow these principles:

1. **Write Clear Tests**: Name clearly, structure well
2. **Test Thoroughly**: Happy path, errors, edges, boundaries
3. **Keep Tests Fast**: < 100ms per test
4. **Maintain Independence**: No test interdependencies
5. **Clean Up Resources**: Always free what you allocate
6. **Assert Properly**: Use specific assertions with messages
7. **Review Carefully**: Use the checklist

Good tests are as important as good code!
