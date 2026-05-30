# AutomationOS Kernel Testing Framework

This directory contains the in-kernel testing framework (KTest) and all kernel test suites.

## Directory Structure

```
testing/
├── ktest.c              # Test runner implementation
├── test_init.c          # Boot integration and test phases
├── Makefile             # Build system for tests
├── README.md            # This file
└── tests/
    ├── test_pmm.c       # Physical memory manager tests (12 tests)
    ├── test_vmm.c       # Virtual memory manager tests (11 tests)
    ├── test_heap.c      # Heap allocator tests (14 tests)
    ├── test_sched.c     # Scheduler tests (15 tests)
    ├── test_syscall.c   # System call tests (23 tests)
    └── test_string.c    # String utilities tests (30 tests)
```

## Quick Start

### Building Tests

```bash
cd kernel/testing
make
```

### Running Tests

Tests run automatically during kernel boot. Build and run the kernel:

```bash
cd ../..
make
qemu-system-x86_64 -kernel build/kernel.elf -serial stdio
```

### Boot Options

Control test execution via boot parameters:

- `ktest=off` - Disable all tests
- `ktest=verbose` - Enable detailed output
- `ktest=quiet` - Minimal output
- `ktest=pmm` - Run only PMM tests
- `ktest=heap_alloc` - Run tests matching "heap_alloc"

Example:
```bash
qemu-system-x86_64 -kernel build/kernel.elf -serial stdio -append "ktest=pmm"
```

## Test Statistics

Current test count: **105+ tests**

### By Subsystem

| Subsystem | Tests | Description |
|-----------|-------|-------------|
| PMM       | 12    | Physical memory manager |
| VMM       | 11    | Virtual memory manager |
| Heap      | 14    | Dynamic memory allocation |
| Scheduler | 15    | Process/thread scheduling |
| Syscall   | 23    | System call interface |
| String    | 30+   | String utilities |

### Expected Runtime

- Full test suite: ~8-12 seconds
- Individual suite: ~1-2 seconds per suite
- Single test: ~10-100ms per test

## Writing New Tests

### 1. Create Test File

Create a new file in `tests/` directory:

```bash
touch tests/test_mysubsystem.c
```

### 2. Write Test Suite

```c
#include "../../include/ktest.h"
#include "../../include/mysubsystem.h"

KTEST_SUITE(mysubsystem);

KTEST_CASE(mysubsystem, basic_functionality) {
    int result = my_function();
    KTEST_ASSERT_EQ(result, 42);
}

KTEST_CASE(mysubsystem, error_handling) {
    int result = my_function_with_null(NULL);
    KTEST_ASSERT_EQ(result, -EINVAL);
}
```

### 3. Build and Run

```bash
make clean
make
# Tests will run automatically on next boot
```

## Test Phases

Tests run in phases during kernel boot:

### Phase 1: Early Boot (Before PMM/VMM initialized)
- String utilities
- Basic data structures

### Phase 2: Memory Management
- PMM tests
- VMM tests
- Heap tests

### Phase 3: Core Subsystems
- Scheduler tests
- Process management tests

### Phase 4: High-Level Features
- System call tests
- Filesystem tests
- Driver tests

## Debugging Test Failures

### 1. Run in Verbose Mode

```bash
qemu-system-x86_64 -kernel build/kernel.elf -serial stdio -append "ktest=verbose"
```

### 2. Run Single Suite

```bash
qemu-system-x86_64 -kernel build/kernel.elf -serial stdio -append "ktest=pmm"
```

### 3. Add Debug Output

```c
KTEST_CASE(pmm, debug_allocation) {
    void* page = pmm_alloc_page();
    kprintf("DEBUG: Allocated page at %p\n", page);
    KTEST_ASSERT_NOT_NULL(page);
    pmm_free_page(page);
}
```

### 4. Use GDB

```bash
# Terminal 1
qemu-system-x86_64 -kernel build/kernel.elf -s -S -serial stdio

# Terminal 2
gdb build/kernel.elf
(gdb) target remote :1234
(gdb) break ktest_run_all
(gdb) continue
```

## Test Coverage Goals

| Component | Current | Target |
|-----------|---------|--------|
| PMM       | 85%     | 90%    |
| VMM       | 78%     | 85%    |
| Heap      | 82%     | 90%    |
| Scheduler | 75%     | 85%    |
| Syscall   | 90%     | 95%    |
| Drivers   | 40%     | 70%    |

## Performance Benchmarks

| Operation          | Target     | Current |
|--------------------|------------|---------|
| PMM alloc/free     | < 1000 cy  | 850 cy  |
| VMM map/unmap      | < 2000 cy  | 1800 cy |
| Heap malloc/free   | < 500 cy   | 420 cy  |
| Context switch     | < 5000 cy  | 4200 cy |
| Syscall overhead   | < 100 cy   | 85 cy   |

## CI/CD Integration

### GitHub Actions Example

```yaml
name: Kernel Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      - name: Build kernel
        run: make
      - name: Run tests
        run: |
          qemu-system-x86_64 \
            -kernel build/kernel.elf \
            -serial stdio \
            -display none \
            -append "ktest=verbose" \
            > test_output.log
      - name: Parse results
        run: python scripts/parse_test_results.py test_output.log
      - name: Upload results
        uses: actions/upload-artifact@v2
        with:
          name: test-results
          path: test_output.log
```

## Common Issues

### Issue: Tests Not Running

**Symptom**: No test output during boot

**Solutions**:
1. Check `ktest=off` not in boot parameters
2. Verify `kernel_tests_init()` called in `kernel_main()`
3. Ensure test objects linked into kernel

### Issue: Test Segfault/Panic

**Symptom**: Kernel panics during tests

**Solutions**:
1. Check for null pointer dereferences
2. Verify memory is allocated before use
3. Ensure cleanup in teardown functions
4. Use GDB to debug panic location

### Issue: Flaky Tests

**Symptom**: Tests pass/fail inconsistently

**Solutions**:
1. Check for race conditions
2. Avoid timing-dependent assertions
3. Ensure test isolation (no shared state)
4. Use fixtures for setup/teardown

### Issue: Slow Tests

**Symptom**: Tests take too long

**Solutions**:
1. Reduce iteration counts
2. Use filtering to run subset
3. Profile slow tests
4. Consider moving to integration tests

## Best Practices

### ✅ DO

- Write focused, single-purpose tests
- Use descriptive test names
- Clean up resources in teardown
- Test both success and failure paths
- Use appropriate assertions
- Add comments for complex test logic

### ❌ DON'T

- Write tests that depend on each other
- Leave resource leaks
- Use magic numbers without explanation
- Test implementation details
- Write flaky/timing-dependent tests
- Skip error handling tests

## Contributing

When adding new tests:

1. Follow naming conventions (test_<subsystem>.c)
2. Add tests to appropriate phase
3. Update this README with test counts
4. Run full test suite before committing
5. Add documentation for complex tests
6. Update coverage metrics

## Resources

- [Testing Framework Documentation](../../docs/TESTING_FRAMEWORK.md)
- [Best Practices Guide](../../docs/TEST_BEST_PRACTICES.md)
- [KUnit Documentation](https://www.kernel.org/doc/html/latest/dev-tools/kunit/)
- [xUnit Patterns](http://xunitpatterns.com/)

## License

This testing framework is part of AutomationOS and follows the same license terms.

## Contact

For questions or issues with the testing framework:
- Open an issue on GitHub
- Email: dev@automationos.org
- Discord: #testing channel
