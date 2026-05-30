# KTest - In-Kernel Testing Framework

## Overview

KTest is AutomationOS's in-kernel testing framework, inspired by Linux KUnit. It allows you to write and run tests in kernel context during boot time, providing comprehensive test coverage for kernel subsystems.

## Features

- **In-Kernel Execution**: Tests run in kernel mode with full hardware access
- **Boot-Time Testing**: Tests execute during kernel initialization
- **Rich Assertions**: Comprehensive assertion library with clear error messages
- **Test Fixtures**: Setup/teardown functions for test isolation
- **Test Discovery**: Automatic test registration and discovery
- **Filtering**: Run specific tests or test suites
- **Benchmarking**: Built-in performance measurement tools
- **Mock Framework**: Function mocking for isolated unit tests
- **Color Output**: Clear, color-coded test results

## Architecture

### Core Components

```
kernel/
├── include/
│   └── ktest.h              # Public API and macros
├── testing/
│   ├── ktest.c              # Test runner implementation
│   └── tests/
│       ├── test_pmm.c       # Physical memory manager tests
│       ├── test_vmm.c       # Virtual memory manager tests
│       ├── test_heap.c      # Heap allocator tests
│       ├── test_sched.c     # Scheduler tests
│       └── test_syscall.c   # System call tests
```

### Test Lifecycle

1. **Registration**: Tests register themselves using `__attribute__((constructor))`
2. **Initialization**: `ktest_init()` prepares the testing framework
3. **Discovery**: Framework discovers all registered test suites
4. **Execution**: Tests run in order, with setup/teardown
5. **Reporting**: Results are printed with statistics

## Writing Tests

### Basic Test

```c
#include "ktest.h"

// Define a test suite
KTEST_SUITE(my_subsystem);

// Define test cases
KTEST_CASE(my_subsystem, basic_functionality) {
    int result = my_function();
    KTEST_ASSERT_EQ(result, 42);
}

KTEST_CASE(my_subsystem, null_pointer_handling) {
    void* ptr = my_allocate_function(100);
    KTEST_ASSERT_NOT_NULL(ptr);
    my_free_function(ptr);
}
```

### Test with Fixtures

```c
// Define fixture structure
typedef struct {
    void* buffer;
    size_t size;
    int fd;
} my_fixture_t;

// Setup function
static void my_setup(my_fixture_t* fixture) {
    fixture->buffer = kmalloc(4096);
    fixture->size = 4096;
    fixture->fd = open_test_file();
}

// Teardown function
static void my_teardown(my_fixture_t* fixture) {
    kfree(fixture->buffer);
    close_test_file(fixture->fd);
}

// Define suite with fixture
KTEST_SUITE_WITH_FIXTURE(my_subsystem, my_fixture_t, my_setup, my_teardown);

// Use fixture in tests
KTEST_CASE(my_subsystem, test_with_fixture) {
    my_fixture_t* f = (my_fixture_t*)fixture;

    // Use fixture data
    write_to_buffer(f->buffer, f->size);
    KTEST_ASSERT_TRUE(buffer_is_valid(f->buffer));
}
```

## Assertion API

### Basic Assertions

```c
KTEST_ASSERT(condition)              // Assert condition is true
KTEST_ASSERT_MSG(cond, "message")    // Assert with custom message
KTEST_ASSERT_TRUE(condition)         // Assert true
KTEST_ASSERT_FALSE(condition)        // Assert false
```

### Equality Assertions

```c
KTEST_ASSERT_EQ(a, b)     // Assert equal
KTEST_ASSERT_NE(a, b)     // Assert not equal
KTEST_ASSERT_LT(a, b)     // Assert less than
KTEST_ASSERT_LE(a, b)     // Assert less than or equal
KTEST_ASSERT_GT(a, b)     // Assert greater than
KTEST_ASSERT_GE(a, b)     // Assert greater than or equal
```

### Pointer Assertions

```c
KTEST_ASSERT_NULL(ptr)         // Assert pointer is NULL
KTEST_ASSERT_NOT_NULL(ptr)     // Assert pointer is not NULL
KTEST_ASSERT_PTR_EQ(p1, p2)    // Assert pointers are equal
```

### String and Memory Assertions

```c
KTEST_ASSERT_STR_EQ(s1, s2)         // Assert strings are equal
KTEST_ASSERT_MEM_EQ(m1, m2, size)   // Assert memory regions are equal
```

### Expect Assertions (Non-Fatal)

```c
KTEST_EXPECT(condition)      // Expect condition (continues on failure)
KTEST_EXPECT_EQ(a, b)       // Expect equal (continues on failure)
```

### Death Tests

```c
KTEST_EXPECT_DEATH({
    dangerous_operation();
});
```

## Running Tests

### Boot-Time Execution

Tests run automatically during kernel boot. Add to `kernel_main()`:

```c
void kernel_main(boot_info_t* boot_info) {
    // ... initialization ...

    // Initialize and run tests
    ktest_init();
    ktest_run_all();

    // ... continue boot ...
}
```

### Configuration Options

Enable/disable testing:

```c
ktest_set_enabled(true);   // Enable testing
ktest_set_enabled(false);  // Disable testing
```

Set verbose mode:

```c
ktest_set_verbose(true);   // Detailed output
ktest_set_verbose(false);  // Minimal output
```

Filter tests:

```c
ktest_set_filter("pmm");        // Run only PMM tests
ktest_set_filter("heap_alloc"); // Run tests matching "heap_alloc"
```

Run specific suite:

```c
ktest_run_suite("pmm");    // Run only PMM test suite
```

### Boot Flags

Control testing via boot parameters:

```
ktest=off           # Disable testing
ktest=verbose       # Enable verbose mode
ktest=pmm           # Run only PMM tests
ktest=quick         # Run only fast tests
```

## Test Organization

### File Naming Convention

- Test files: `test_<subsystem>.c`
- One test file per subsystem
- Located in `kernel/testing/tests/`

### Suite Naming Convention

- Suite name should match subsystem: `KTEST_SUITE(pmm)`
- Test case names should be descriptive: `alloc_returns_non_null_page`

### Test Categories

1. **Unit Tests**: Test individual functions in isolation
2. **Integration Tests**: Test interactions between subsystems
3. **Regression Tests**: Prevent known bugs from reoccurring
4. **Performance Tests**: Ensure acceptable performance
5. **Stress Tests**: Test under heavy load

## Benchmarking

```c
KTEST_CASE(my_subsystem, performance_test) {
    ktest_benchmark_start();

    // Code to benchmark
    for (int i = 0; i < 1000; i++) {
        my_function();
    }

    uint64_t cycles = ktest_benchmark_end("1000 function calls");

    // Assert performance requirements
    KTEST_ASSERT_LT(cycles, 1000000);  // Less than 1M cycles
}
```

## Mock Framework

```c
// Define mock
static void* original_function(void* args);
static void* mock_function(void* args);

ktest_mock_t my_mock = {
    .name = "my_function",
    .original = original_function,
    .mock = mock_function,
    .call_count = 0
};

KTEST_CASE(my_subsystem, test_with_mock) {
    // Install mock
    ktest_mock_function(&my_mock);

    // Call code that uses the function
    my_code_under_test();

    // Verify mock was called
    KTEST_ASSERT_EQ(ktest_get_call_count(&my_mock), 1);

    // Restore original
    ktest_unmock_function(&my_mock);
}
```

## Test Output

### Example Output

```
====================================
   KTest Framework v1.0
   In-Kernel Testing System
====================================

[=====] Running tests from 5 suites

[----- ] pmm
  [RUN ] pmm.init_calculates_correct_total_memory
  [ OK ] pmm.init_calculates_correct_total_memory (2341 cycles)
  [RUN ] pmm.alloc_returns_non_null_page
  [ OK ] pmm.alloc_returns_non_null_page (1872 cycles)
  [RUN ] pmm.alloc_returns_aligned_pages
  [ OK ] pmm.alloc_returns_aligned_pages (1923 cycles)

[----- ] vmm
  [RUN ] vmm.map_page_succeeds
  [ OK ] vmm.map_page_succeeds (4521 cycles)
  [RUN ] vmm.unmap_page_succeeds
  [ OK ] vmm.unmap_page_succeeds (3892 cycles)

====================================
   Test Summary
====================================
Total:   127 tests
Passed:  125 tests
Failed:  2 tests
Time:    892341 cycles
====================================

All tests PASSED!
```

## Best Practices

### 1. Test Isolation

- Each test should be independent
- Use fixtures for setup/teardown
- Don't rely on execution order

### 2. Clear Test Names

```c
// Good
KTEST_CASE(pmm, alloc_returns_aligned_pages)
KTEST_CASE(vmm, unmap_non_mapped_page_is_safe)

// Bad
KTEST_CASE(pmm, test1)
KTEST_CASE(vmm, test_case_2)
```

### 3. One Assertion per Test (Generally)

```c
// Good - focused test
KTEST_CASE(heap, malloc_returns_non_null) {
    void* ptr = kmalloc(64);
    KTEST_ASSERT_NOT_NULL(ptr);
    kfree(ptr);
}

// Acceptable - related assertions
KTEST_CASE(heap, malloc_returns_aligned_memory) {
    void* ptr = kmalloc(128);
    KTEST_ASSERT_NOT_NULL(ptr);
    KTEST_ASSERT_EQ((uintptr_t)ptr % 8, 0);
    kfree(ptr);
}
```

### 4. Test Both Success and Failure Paths

```c
KTEST_CASE(vmm, map_page_succeeds) {
    // Test success case
}

KTEST_CASE(vmm, map_page_fails_with_invalid_address) {
    // Test failure case
}
```

### 5. Clean Up Resources

```c
KTEST_CASE(pmm, test_with_cleanup) {
    void* page = pmm_alloc_page();
    KTEST_ASSERT_NOT_NULL(page);

    // ... test code ...

    pmm_free_page(page);  // Always clean up
}
```

### 6. Use Descriptive Failure Messages

```c
KTEST_ASSERT_MSG(page_count > 0, "No pages available for allocation");
KTEST_ASSERT_MSG(addr % PAGE_SIZE == 0, "Address is not page-aligned");
```

## Common Patterns

### Testing Null Pointer Handling

```c
KTEST_CASE(subsystem, handles_null_pointer) {
    int result = my_function(NULL);
    KTEST_ASSERT_EQ(result, -EINVAL);
}
```

### Testing Boundary Conditions

```c
KTEST_CASE(subsystem, handles_zero_size) {
    void* ptr = my_alloc(0);
    // Should return NULL or valid pointer
    if (ptr) kfree(ptr);
    KTEST_ASSERT_TRUE(true);
}
```

### Testing Resource Limits

```c
KTEST_CASE(subsystem, respects_max_limit) {
    for (int i = 0; i < MAX_ITEMS + 1; i++) {
        void* item = allocate_item();
        if (i < MAX_ITEMS) {
            KTEST_ASSERT_NOT_NULL(item);
        } else {
            KTEST_ASSERT_NULL(item);  // Should fail after limit
        }
    }
}
```

## Debugging Failed Tests

### 1. Enable Verbose Mode

```c
ktest_set_verbose(true);
```

### 2. Run Single Test Suite

```c
ktest_run_suite("pmm");
```

### 3. Add Debug Output

```c
KTEST_CASE(pmm, debug_test) {
    void* page = pmm_alloc_page();
    kprintf("Allocated page at: %p\n", page);
    KTEST_ASSERT_NOT_NULL(page);
}
```

### 4. Use Expect Instead of Assert

```c
// This will continue executing and show all failures
KTEST_EXPECT_EQ(value1, expected1);
KTEST_EXPECT_EQ(value2, expected2);
KTEST_EXPECT_EQ(value3, expected3);
```

## Performance Considerations

- Tests add ~5-10 seconds to boot time
- Use `ktest=off` in production
- Disable slow tests with filtering
- Keep tests focused and fast

## Integration with CI/CD

### Automated Testing

```bash
# Run tests in QEMU
qemu-system-x86_64 -kernel kernel.elf -serial stdio

# Parse test output
python parse_test_results.py < kernel.log

# Generate JUnit XML
python ktest_to_junit.py kernel.log > test-results.xml
```

### Test Coverage

```bash
# Build with coverage
make COVERAGE=1

# Run tests
qemu-system-x86_64 -kernel kernel.elf

# Generate coverage report
gcov kernel/**/*.c
```

## Future Enhancements

- [ ] Parallel test execution
- [ ] Test result caching
- [ ] Coverage-guided fuzzing integration
- [ ] Graphical test report
- [ ] Remote test execution
- [ ] Test result history tracking
- [ ] Automatic test generation
- [ ] Performance regression detection

## Troubleshooting

### Tests Not Running

- Check `ktest_init()` is called
- Verify boot flags aren't disabling tests
- Ensure test files are compiled and linked

### Tests Crashing

- Use simpler assertions first
- Check for memory corruption
- Verify fixtures are properly initialized

### Tests Taking Too Long

- Use filtering to run subset
- Optimize expensive operations
- Consider moving to integration tests

## References

- Linux KUnit Documentation: https://www.kernel.org/doc/html/latest/dev-tools/kunit/
- Google Test: https://github.com/google/googletest
- xUnit Test Patterns: http://xunitpatterns.com/

## License

This testing framework is part of AutomationOS and follows the same license.
