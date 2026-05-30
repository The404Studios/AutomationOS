# KTest Integration Guide

This guide shows how to integrate the KTest framework into AutomationOS kernel.

## Step 1: Update kernel.h

Add test framework declarations to `kernel/include/kernel.h`:

```c
#ifndef KERNEL_H
#define KERNEL_H

#include "types.h"

// ... existing declarations ...

// Test framework functions
void kernel_tests_init(void);
void kernel_tests_run(void);
void kernel_tests_run_phase(int phase);
bool kernel_tests_enabled(void);
void kernel_tests_set_enabled(bool enabled);

#endif
```

## Step 2: Update kernel_main()

Modify `kernel/kernel.c` to initialize and run tests:

```c
#include "include/kernel.h"
#include "include/mem.h"
#include "include/drivers.h"
#include "include/x86_64.h"
#include "include/syscall.h"
#include "include/perf.h"

void kernel_main(boot_info_t* boot_info) {
    // Start total boot time measurement
    uint64_t boot_start = rdtsc();

    // Initialize serial console first for debug output
    serial_init();

    kprintf("\n");
    kprintf("=====================================\n");
    kprintf("   AutomationOS v%d.%d.%d\n",
            KERNEL_VERSION_MAJOR,
            KERNEL_VERSION_MINOR,
            KERNEL_VERSION_PATCH);
    kprintf("=====================================\n");
    kprintf("\n");

    // Initialize test framework early
    kernel_tests_init();

    // Initialize GDT
    PERF_TIMER_START();
    gdt_init();
    PERF_TIMER_END("gdt_init");

    // Initialize physical memory manager
    PERF_TIMER_START();
    pmm_init(boot_info->memory_map, boot_info->memory_map_count);
    PERF_TIMER_END("pmm_init");

    // Run early boot tests (PMM)
    if (kernel_tests_enabled()) {
        kernel_tests_run_phase(TEST_PHASE_EARLY);
    }

    // Initialize virtual memory manager
    PERF_TIMER_START();
    vmm_init();
    PERF_TIMER_END("vmm_init");

    // Initialize kernel heap
    PERF_TIMER_START();
    heap_init();
    PERF_TIMER_END("heap_init");

    // Run memory management tests
    if (kernel_tests_enabled()) {
        kernel_tests_run_phase(TEST_PHASE_MIDDLE);
    }

    // Initialize IDT and interrupts
    PERF_TIMER_START();
    idt_init();
    PERF_TIMER_END("idt_init");

    // Initialize SYSCALL/SYSRET MSRs
    PERF_TIMER_START();
    syscall_msr_init();
    PERF_TIMER_END("syscall_msr_init");

    // Initialize syscall handler table
    PERF_TIMER_START();
    syscall_init();
    PERF_TIMER_END("syscall_init");

    // Run late boot tests (syscalls, scheduler, etc.)
    if (kernel_tests_enabled()) {
        kernel_tests_run_phase(TEST_PHASE_LATE);
    }

    // ... rest of initialization ...

    // Calculate and print total boot time
    uint64_t boot_end = rdtsc();
    uint64_t boot_cycles = boot_end - boot_start;
    kprintf("\n");
    kprintf("Boot complete in %llu cycles\n", boot_cycles);

    // Export test results
    if (kernel_tests_enabled()) {
        kernel_tests_export_results();
    }

    kprintf("\n");

    // Enter main kernel loop or idle
    while (1) {
        asm volatile("hlt");
    }
}
```

## Step 3: Update Makefile

Add test framework to kernel build. Update `Makefile`:

```makefile
# Kernel objects
KERNEL_OBJS = kernel/kernel.o \
              kernel/arch/x86_64/gdt.o \
              kernel/arch/x86_64/idt.o \
              kernel/core/mem/pmm.o \
              kernel/core/mem/vmm.o \
              kernel/core/mem/heap.o \
              # ... other objects ...

# Test framework objects
TEST_OBJS = kernel/testing/ktest.o \
            kernel/testing/test_init.o \
            kernel/testing/tests/test_pmm.o \
            kernel/testing/tests/test_vmm.o \
            kernel/testing/tests/test_heap.o \
            kernel/testing/tests/test_sched.o \
            kernel/testing/tests/test_syscall.o \
            kernel/testing/tests/test_string.o

# Conditional compilation - disable tests in release builds
ifdef RELEASE
    # Don't include test objects in release build
    ALL_OBJS = $(KERNEL_OBJS)
else
    # Include tests in debug build
    ALL_OBJS = $(KERNEL_OBJS) $(TEST_OBJS)
endif

kernel.elf: $(ALL_OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

# Build test framework
kernel/testing/%.o: kernel/testing/%.c
	$(CC) $(CFLAGS) -c $< -o $@

kernel/testing/tests/%.o: kernel/testing/tests/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Test targets
.PHONY: test test-run test-verbose test-suite

test: kernel.elf
	qemu-system-x86_64 -kernel $< -serial stdio -display none

test-run: kernel.elf
	qemu-system-x86_64 -kernel $< -serial stdio -display none > test_output.log
	python3 scripts/parse_test_results.py test_output.log --junit test-results.xml --html test-report.html

test-verbose: kernel.elf
	qemu-system-x86_64 -kernel $< -serial stdio -display none -append "ktest=verbose"

test-suite: kernel.elf
	@echo "Running $(SUITE) test suite..."
	qemu-system-x86_64 -kernel $< -serial stdio -display none -append "ktest=$(SUITE)"

# Build release version without tests
release: CFLAGS += -DRELEASE -O3
release: RELEASE=1
release: clean kernel.elf
```

## Step 4: Boot Parameter Parsing

Add boot parameter parsing to extract test configuration. Create or update `kernel/core/bootparam.c`:

```c
#include "../include/kernel.h"

// Parse kernel command line
void parse_boot_params(const char* cmdline) {
    if (!cmdline) {
        return;
    }

    // Parse test parameters
    parse_test_boot_params(cmdline);

    // Parse other parameters...
}
```

Call this from `kernel_main()` if boot parameters are available:

```c
void kernel_main(boot_info_t* boot_info) {
    // ... initialization ...

    // Parse boot parameters
    if (boot_info->cmdline) {
        parse_boot_params(boot_info->cmdline);
    }

    // Initialize tests (will respect parsed parameters)
    kernel_tests_init();

    // ... rest of boot ...
}
```

## Step 5: Add Test Hooks to Panic Handler

Update panic handler to report test context. In `kernel/panic.c`:

```c
#include "../include/kernel.h"

void kernel_panic(const char* message) {
    // Disable interrupts
    asm volatile("cli");

    kprintf("\n\n");
    kprintf("*** KERNEL PANIC ***\n");
    kprintf("Reason: %s\n", message);

    // Report test context if in testing mode
    if (kernel_tests_enabled()) {
        kernel_tests_on_panic();
    }

    // Print stack trace, registers, etc.
    // ...

    // Halt
    while (1) {
        asm volatile("hlt");
    }
}
```

## Step 6: Create Test Configuration Header

Create `kernel/include/ktest_config.h` for test configuration:

```c
#ifndef KTEST_CONFIG_H
#define KTEST_CONFIG_H

// Enable/disable test framework at compile time
#ifndef KTEST_ENABLED
#define KTEST_ENABLED 1
#endif

// Maximum number of test suites
#define KTEST_MAX_SUITES 64

// Maximum fixture size (bytes)
#define KTEST_MAX_FIXTURE_SIZE 4096

// Enable color output
#define KTEST_COLOR_OUTPUT 1

// Enable benchmarking
#define KTEST_BENCHMARKING 1

// Enable mocking framework
#define KTEST_MOCKING 1

// Test timeout (cycles)
#define KTEST_TIMEOUT_CYCLES 1000000000ULL

#endif
```

## Step 7: Add Conditional Compilation Guards

Update test files to support conditional compilation:

```c
#include "../../include/ktest.h"

#if KTEST_ENABLED

KTEST_SUITE(pmm);

KTEST_CASE(pmm, alloc_returns_non_null) {
    // Test code...
}

// ... more tests ...

#endif // KTEST_ENABLED
```

## Step 8: Create Test Runner Script

Create `scripts/run_tests.sh`:

```bash
#!/bin/bash

# AutomationOS Kernel Test Runner

set -e

KERNEL="build/kernel.elf"
TIMEOUT=30
OUTPUT_DIR="test-results"

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Build kernel with tests
echo "Building kernel with test framework..."
make clean
make

# Run tests
echo "Running kernel tests..."
timeout $TIMEOUT qemu-system-x86_64 \
    -kernel "$KERNEL" \
    -serial stdio \
    -display none \
    -append "ktest=verbose" \
    > "$OUTPUT_DIR/test_output.log" || true

# Parse results
echo "Parsing test results..."
python3 scripts/parse_test_results.py \
    "$OUTPUT_DIR/test_output.log" \
    --junit "$OUTPUT_DIR/test-results.xml" \
    --json "$OUTPUT_DIR/test-results.json" \
    --html "$OUTPUT_DIR/test-report.html"

echo "Test execution complete!"
echo "Results:"
echo "  - Log:  $OUTPUT_DIR/test_output.log"
echo "  - XML:  $OUTPUT_DIR/test-results.xml"
echo "  - JSON: $OUTPUT_DIR/test-results.json"
echo "  - HTML: $OUTPUT_DIR/test-report.html"
```

Make it executable:
```bash
chmod +x scripts/run_tests.sh
```

## Step 9: Add CI/CD Integration

Create `.github/workflows/tests.yml`:

```yaml
name: Kernel Tests

on:
  push:
    branches: [ main, develop ]
  pull_request:
    branches: [ main, develop ]

jobs:
  test:
    runs-on: ubuntu-latest

    steps:
    - uses: actions/checkout@v3

    - name: Install dependencies
      run: |
        sudo apt-get update
        sudo apt-get install -y qemu-system-x86 build-essential nasm python3

    - name: Build kernel
      run: make

    - name: Run tests
      run: ./scripts/run_tests.sh

    - name: Upload test results
      if: always()
      uses: actions/upload-artifact@v3
      with:
        name: test-results
        path: test-results/

    - name: Publish test report
      if: always()
      uses: dorny/test-reporter@v1
      with:
        name: Kernel Test Results
        path: test-results/test-results.xml
        reporter: java-junit

    - name: Comment PR with results
      if: github.event_name == 'pull_request'
      uses: actions/github-script@v6
      with:
        script: |
          const fs = require('fs');
          const results = JSON.parse(fs.readFileSync('test-results/test-results.json'));

          const body = `## 🧪 Kernel Test Results

          | Metric | Value |
          |--------|-------|
          | Total Tests | ${results.summary.total} |
          | ✅ Passed | ${results.summary.passed} |
          | ❌ Failed | ${results.summary.failed} |
          | ⏭️ Skipped | ${results.summary.skipped} |
          | 📊 Pass Rate | ${results.summary.pass_rate.toFixed(1)}% |

          [View full report](test-report.html)
          `;

          github.rest.issues.createComment({
            issue_number: context.issue.number,
            owner: context.repo.owner,
            repo: context.repo.repo,
            body: body
          });
```

## Step 10: Usage Examples

### Run all tests:
```bash
make test
```

### Run specific suite:
```bash
make test-suite SUITE=pmm
```

### Run with verbose output:
```bash
make test-verbose
```

### Generate reports:
```bash
./scripts/run_tests.sh
```

### Manual QEMU invocation:
```bash
qemu-system-x86_64 -kernel build/kernel.elf -serial stdio -append "ktest=verbose"
```

## Troubleshooting

### Tests not running

1. Check that `KTEST_ENABLED` is defined
2. Verify test objects are linked into kernel
3. Ensure `kernel_tests_init()` is called

### Tests causing panics

1. Run individual suite: `make test-suite SUITE=problematic_suite`
2. Use GDB: `qemu-system-x86_64 -kernel build/kernel.elf -s -S`
3. Check test setup/teardown functions

### Slow test execution

1. Use filtering: `qemu ... -append "ktest=fast_suite"`
2. Reduce iteration counts in stress tests
3. Build release version: `make RELEASE=1`

## Best Practices

1. **Run tests before committing**: Always run full test suite
2. **Write tests alongside code**: TDD approach
3. **Keep tests fast**: Each test < 100ms
4. **Use fixtures**: Avoid duplication in setup
5. **Test isolation**: No dependencies between tests
6. **Meaningful names**: Describe what is being tested
7. **Clean up resources**: Prevent leaks in tests

## Next Steps

1. Add more test suites for untested subsystems
2. Increase test coverage to 90%+
3. Add performance regression tests
4. Integrate with coverage tools (gcov)
5. Add fuzzing integration
6. Create test templates for common patterns
7. Document test writing guidelines
8. Set up nightly test runs

## References

- [KTest API Documentation](../../docs/TESTING_FRAMEWORK.md)
- [Test Best Practices](../../docs/TEST_BEST_PRACTICES.md)
- [Linux KUnit](https://www.kernel.org/doc/html/latest/dev-tools/kunit/)
