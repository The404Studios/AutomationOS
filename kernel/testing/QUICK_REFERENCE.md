# KTest Quick Reference Card

## Writing Tests

### Basic Test
```c
KTEST_SUITE(my_subsystem);

KTEST_CASE(my_subsystem, test_name) {
    int result = my_function();
    KTEST_ASSERT_EQ(result, 42);
}
```

### Test with Fixture
```c
typedef struct {
    void* buffer;
    size_t size;
} my_fixture_t;

static void setup(my_fixture_t* f) {
    f->buffer = kmalloc(4096);
    f->size = 4096;
}

static void teardown(my_fixture_t* f) {
    kfree(f->buffer);
}

KTEST_SUITE_WITH_FIXTURE(my_subsystem, my_fixture_t, setup, teardown);

KTEST_CASE(my_subsystem, test_with_fixture) {
    my_fixture_t* f = (my_fixture_t*)fixture;
    // Use f->buffer, f->size
}
```

## Assertions

| Assertion | Description |
|-----------|-------------|
| `KTEST_ASSERT(cond)` | Assert condition is true |
| `KTEST_ASSERT_TRUE(cond)` | Assert true |
| `KTEST_ASSERT_FALSE(cond)` | Assert false |
| `KTEST_ASSERT_EQ(a, b)` | Assert equal |
| `KTEST_ASSERT_NE(a, b)` | Assert not equal |
| `KTEST_ASSERT_LT(a, b)` | Assert less than |
| `KTEST_ASSERT_LE(a, b)` | Assert less than or equal |
| `KTEST_ASSERT_GT(a, b)` | Assert greater than |
| `KTEST_ASSERT_GE(a, b)` | Assert greater than or equal |
| `KTEST_ASSERT_NULL(ptr)` | Assert pointer is NULL |
| `KTEST_ASSERT_NOT_NULL(ptr)` | Assert pointer is not NULL |
| `KTEST_ASSERT_PTR_EQ(p1, p2)` | Assert pointers are equal |
| `KTEST_ASSERT_STR_EQ(s1, s2)` | Assert strings are equal |
| `KTEST_ASSERT_MEM_EQ(m1, m2, n)` | Assert memory regions equal |
| `KTEST_EXPECT(cond)` | Expect (non-fatal) |
| `KTEST_EXPECT_EQ(a, b)` | Expect equal (non-fatal) |

## Running Tests

### Command Line
```bash
# Build with tests
make

# Run all tests
make test

# Run specific suite
make test-suite SUITE=pmm

# Verbose output
make test-verbose

# Generate reports
./scripts/run_tests.sh
```

### QEMU Options
```bash
# All tests
qemu-system-x86_64 -kernel kernel.elf -serial stdio

# Verbose mode
qemu-system-x86_64 -kernel kernel.elf -serial stdio -append "ktest=verbose"

# Specific suite
qemu-system-x86_64 -kernel kernel.elf -serial stdio -append "ktest=pmm"

# Disable tests
qemu-system-x86_64 -kernel kernel.elf -serial stdio -append "ktest=off"
```

## API Functions

| Function | Description |
|----------|-------------|
| `ktest_init()` | Initialize framework |
| `ktest_run_all()` | Run all tests |
| `ktest_run_suite(name)` | Run specific suite |
| `ktest_set_enabled(bool)` | Enable/disable testing |
| `ktest_set_verbose(bool)` | Set verbose mode |
| `ktest_set_filter(filter)` | Set test filter |
| `ktest_get_stats()` | Get test statistics |

## Benchmarking

```c
KTEST_CASE(my_subsystem, performance_test) {
    ktest_benchmark_start();

    // Code to benchmark
    for (int i = 0; i < 1000; i++) {
        my_function();
    }

    uint64_t cycles = ktest_benchmark_end("1000 iterations");
    KTEST_ASSERT_LT(cycles, 1000000);
}
```

## Mocking

```c
ktest_mock_t my_mock = {
    .name = "my_function",
    .original = original_function,
    .mock = mock_function
};

KTEST_CASE(my_subsystem, test_with_mock) {
    ktest_mock_function(&my_mock);

    // Test code using mocked function

    KTEST_ASSERT_EQ(ktest_get_call_count(&my_mock), 1);
    ktest_unmock_function(&my_mock);
}
```

## Boot Integration

```c
void kernel_main(boot_info_t* boot_info) {
    // Early init...

    kernel_tests_init();

    // After PMM/VMM init
    kernel_tests_run_phase(TEST_PHASE_EARLY);

    // More init...

    // After syscall/scheduler init
    kernel_tests_run_phase(TEST_PHASE_LATE);

    // Final summary
    kernel_tests_export_results();
}
```

## File Structure

```
kernel/testing/
├── ktest.c                 # Framework implementation
├── test_init.c             # Boot integration
├── tests/
│   ├── test_pmm.c         # PMM tests
│   ├── test_vmm.c         # VMM tests
│   ├── test_heap.c        # Heap tests
│   ├── test_sched.c       # Scheduler tests
│   ├── test_syscall.c     # Syscall tests
│   └── test_string.c      # String tests
├── Makefile               # Build system
└── README.md             # Documentation
```

## Test Phases

| Phase | When | Tests |
|-------|------|-------|
| EARLY | After PMM/VMM | pmm, vmm, string |
| MIDDLE | After heap/sched | heap, sched |
| LATE | After syscall | syscall |
| ALL | Any time | All tests |

## Output Format

```
[RUN ] suite.test_name
[ OK ] suite.test_name (1234 cycles)
[FAIL] suite.test_name
       file.c:42: Assertion failed
[SKIP] suite.test_name
```

## Test Statistics

```
====================================
   Test Summary
====================================
Total:   127 tests
Passed:  125 tests
Failed:  2 tests
Skipped: 0 tests
Time:    892341 cycles
====================================
```

## Common Patterns

### Test NULL handling
```c
KTEST_CASE(subsystem, handles_null) {
    int result = my_function(NULL);
    KTEST_ASSERT_EQ(result, -EINVAL);
}
```

### Test resource cleanup
```c
KTEST_CASE(subsystem, frees_memory) {
    uint64_t before = get_used_memory();
    void* ptr = my_alloc();
    my_free(ptr);
    uint64_t after = get_used_memory();
    KTEST_ASSERT_EQ(after, before);
}
```

### Test error codes
```c
KTEST_CASE(subsystem, returns_error) {
    int result = my_function_with_error();
    KTEST_ASSERT_LT(result, 0);
}
```

### Test boundaries
```c
KTEST_CASE(subsystem, boundary_test) {
    KTEST_ASSERT_EQ(my_function(0), expected_min);
    KTEST_ASSERT_EQ(my_function(MAX), expected_max);
}
```

## Troubleshooting

| Problem | Solution |
|---------|----------|
| Tests not running | Check `ktest=off` not set |
| Test crashes | Use GDB: `-s -S` |
| Flaky tests | Check for race conditions |
| Slow tests | Reduce iterations |
| Test leaks | Use fixtures |

## Tips

1. ✅ Write test before code (TDD)
2. ✅ Keep tests small and focused
3. ✅ Use descriptive names
4. ✅ Test both success and failure
5. ✅ Clean up resources
6. ❌ Don't test implementation
7. ❌ Don't make tests depend on each other
8. ❌ Don't skip error cases

## Quick Start Checklist

- [ ] Create test file: `tests/test_mysubsystem.c`
- [ ] Define suite: `KTEST_SUITE(mysubsystem)`
- [ ] Write test cases: `KTEST_CASE(mysubsystem, test_name)`
- [ ] Add assertions: `KTEST_ASSERT_*`
- [ ] Build: `make`
- [ ] Run: `make test`
- [ ] Check results: All passed!

## Resources

- [Full Documentation](../../../docs/TESTING_FRAMEWORK.md)
- [Best Practices](../../../docs/TEST_BEST_PRACTICES.md)
- [Integration Guide](INTEGRATION_GUIDE.md)
- [Linux KUnit](https://kernel.org/doc/html/latest/dev-tools/kunit/)
