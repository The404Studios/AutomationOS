# AutomationOS Stress Test & Chaos Engineering Suite

Comprehensive testing framework to find breaking points and vulnerabilities in the AutomationOS kernel.

## Overview

This suite implements three categories of tests:

1. **Stress Tests** - Find resource limits and breaking points
2. **Chaos Engineering** - Test resilience under random failures
3. **Race Condition Fuzzing** - Expose concurrency bugs

## Quick Start

```bash
# Build all tests
make

# Run complete test suite
make test-all

# Generate visual report
python3 stress_dashboard.py
```

## Test Categories

### 1. Stress Tests (`stress_test_suite.c`)

Tests system behavior under extreme load:

- **Process Bomb** - Create thousands of processes
- **Memory Bomb** - Allocate until OOM
- **Heap Fragmentation** - Fragment memory allocator
- **Integer Overflow** - Test size validation
- **Concurrent Allocations** - Lock contention
- **Resource Limits** - Verify rlimit enforcement
- **Double Free** - Memory safety checks
- **NULL Handling** - Input validation

**Run:** `make stress-test`

### 2. Chaos Engineering (`chaos_engineering.c`)

Injects random failures to test resilience:

- **Random Process Kills** - Terminate processes randomly
- **Memory Pressure** - Random allocation/deallocation
- **Resource Starvation** - Selective resource exhaustion
- **Interrupt Storm** - High interrupt load
- **Lock Contention** - Synchronization stress
- **Combined Failures** - Multiple simultaneous failures
- **Timing Attacks** - Detect timing side-channels
- **Edge Case Fuzzing** - Boundary conditions

**Run:** `make chaos-test`

### 3. Race Condition Fuzzer (`race_condition_fuzzer.c`)

Attempts to trigger concurrency bugs:

- **Reference Count Races** - Use-after-free detection
- **Heap Allocation Races** - Double allocation
- **Process Table Races** - PID conflicts
- **TOCTOU Races** - Check-use gaps
- **Lock-Free Races** - Atomic operation correctness
- **Shared Resource Races** - Lock effectiveness
- **Double-Destroy Races** - Double-free protection
- **ABA Problem** - Pointer reuse issues
- **Scheduler Races** - State transition races
- **Memory Ordering** - Reordering bugs

**Run:** `make race-test`

## File Structure

```
tests/stress/
├── Makefile                      # Build system
├── README.md                     # This file
├── STRESS_TEST_REPORT.md         # Detailed findings
├── stress_test_suite.c           # Main stress tests
├── chaos_engineering.c           # Chaos tests
├── race_condition_fuzzer.c       # Race condition tests
├── stress_dashboard.py           # Report generator
└── *.log                         # Test results (generated)
```

## Building

### Prerequisites

- GCC or Clang
- Make
- Python 3 (for dashboard)

### Build Commands

```bash
# Build all tests
make

# Build specific test
make stress_test_suite
make chaos_engineering
make race_condition_fuzzer

# Clean build artifacts
make clean
```

## Running Tests

### Individual Tests

```bash
# Stress tests only
make stress-test

# Chaos tests only
make chaos-test

# Race condition tests only
make race-test
```

### Complete Suite

```bash
# Run all tests and generate reports
make test-all
```

This will:
1. Build all test executables
2. Run stress tests → `stress_test_results.log`
3. Run chaos tests → `chaos_test_results.log`
4. Run race tests → `race_test_results.log`

### With QEMU

```bash
# Run tests in emulator (if kernel image available)
make qemu-stress
```

## Generating Reports

### Text Summary

```bash
python3 stress_dashboard.py
```

Generates:
- `stress_test_summary.txt` - Text report
- `stress_test_results.json` - Machine-readable results
- `stress_test_dashboard.html` - Visual dashboard

### View HTML Dashboard

```bash
# Linux/Mac
open stress_test_dashboard.html

# Windows
start stress_test_dashboard.html
```

## Interpreting Results

### Test Status

- `[PASS]` - Test passed, system behaved correctly
- `[FAIL]` - Test failed, vulnerability detected

### Severity Levels

- 🔴 **CRITICAL** - Immediate security/stability risk
- 🟠 **HIGH** - Significant issue requiring fix
- 🟡 **MEDIUM** - Should be addressed
- 🔵 **LOW** - Minor issue or improvement

### Breaking Points

Each test reports the point at which system fails:

```
[STRESS] Created 256 processes before limit
Breaking Point: 256
```

### Example Output

```
========================================
  Stress Test Results Summary
========================================

[PASS] Process Bomb
  Breaking Point: 256

[FAIL] Memory Bomb
  Failure Mode: System panic on OOM

[FAIL] Double Free Detection
  Failure Mode: No double-free protection

Total: 8 tests
Passed: 5
Failed: 3

CRITICAL VULNERABILITIES FOUND:
1. No double-free detection
2. No heap canaries/guards
3. System panic on OOM
```

## Critical Vulnerabilities Found

The test suite has identified several critical issues:

### 🔴 CRITICAL

1. **No Double-Free Detection**
   - `kfree()` doesn't validate if memory already freed
   - Can corrupt heap structures

2. **No Heap Canaries**
   - Buffer overflows go undetected
   - Heap metadata can be silently corrupted

3. **System Panic on OOM**
   - Kernel panics instead of graceful handling
   - No OOM killer implementation

### 🟠 HIGH

4. **Process Table Hard Limit**
   - Only 256 processes supported
   - No PID recycling
   - System panic when full

5. **Lock Contention**
   - Single global heap lock
   - 100x variance in allocation time

6. **TOCTOU Vulnerabilities**
   - Race windows in process lookup
   - Can lead to use-after-free

See `STRESS_TEST_REPORT.md` for complete analysis.

## Fixing Vulnerabilities

### Priority 1: Security (Immediate)

```c
// 1. Add double-free detection
void kfree(void* ptr) {
    block_t* block = GET_BLOCK(ptr);
    
    if (block->is_free) {
        kernel_panic("Double-free detected!");
    }
    
    block->is_free = true;
    // ... rest of free logic
}

// 2. Add heap canaries
typedef struct block {
    uint32_t magic;         // 0xDEADBEEF
    uint32_t canary_front;  // Random value
    // ... data ...
    // uint32_t canary_back; (at end)
} block_t;
```

### Priority 2: Reliability

```c
// 3. Dynamic process table
static process_t** process_table = NULL;
static size_t table_size = 0;

void expand_process_table(void) {
    size_t new_size = table_size * 2;
    process_table = realloc(process_table, new_size);
    table_size = new_size;
}

// 4. OOM handling instead of panic
void* kmalloc(size_t size) {
    // ... try allocation ...
    
    if (!ptr) {
        // Try OOM killer
        oom_kill_process();
        return NULL; // Graceful failure
    }
}
```

## Continuous Testing

### CI/CD Integration

Add to `.github/workflows/stress-test.yml`:

```yaml
name: Stress Tests

on: [push, pull_request]

jobs:
  stress:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      - name: Build Kernel
        run: make
      - name: Run Stress Tests
        run: |
          cd tests/stress
          make test-all
      - name: Generate Report
        run: python3 stress_dashboard.py
      - name: Upload Results
        uses: actions/upload-artifact@v2
        with:
          name: stress-test-results
          path: tests/stress/*.html
```

### Automated Monitoring

Integrate chaos metrics into production kernel:

```c
// Kernel metrics
typedef struct {
    uint64_t alloc_failures;
    uint64_t oom_events;
    uint64_t process_failures;
} chaos_metrics_t;

// Track failures
void chaos_record_alloc_failure(void) {
    metrics.alloc_failures++;
}

// Periodic reporting
void report_chaos_metrics(void) {
    kprintf("Chaos Metrics:\n");
    kprintf("  Alloc Failures: %llu\n", metrics.alloc_failures);
    kprintf("  OOM Events: %llu\n", metrics.oom_events);
}
```

## Advanced Usage

### Custom Stress Test

Add new test to `stress_test_suite.c`:

```c
void stress_test_my_feature(void) {
    kprintf("\n[STRESS] === My Feature Test ===\n");
    uint64_t start = rdtsc();
    
    // Your stress test logic here
    
    test_results[test_count].test_name = "My Feature";
    test_results[test_count].passed = true;
    test_results[test_count].duration_cycles = rdtsc() - start;
    test_count++;
}

// Add to run_stress_tests()
void run_stress_tests(void) {
    // ... existing tests ...
    stress_test_my_feature();
    // ...
}
```

### Fuzzing Integration

Use AFL++ for coverage-guided fuzzing:

```bash
# Build with AFL instrumentation
CC=afl-gcc make

# Run fuzzer
afl-fuzz -i testcases/ -o findings/ ./stress_test_suite
```

### Performance Profiling

Add profiling to tests:

```c
#define PROFILE_START(name) \
    uint64_t __prof_##name = rdtsc();

#define PROFILE_END(name) \
    kprintf("[PROFILE] %s: %llu cycles\n", \
            #name, rdtsc() - __prof_##name);

// Usage
PROFILE_START(my_operation);
my_operation();
PROFILE_END(my_operation);
```

## Troubleshooting

### Test Hangs

If test hangs, it likely hit an infinite loop or deadlock:

```bash
# Run with timeout
timeout 30s ./stress_test_suite
```

### Kernel Panic

If test causes kernel panic, check logs:

```bash
# QEMU serial output
qemu-system-x86_64 -kernel kernel.elf -serial stdio
```

### Build Errors

Ensure kernel headers are up to date:

```bash
# Rebuild kernel
cd ../..
make clean
make
```

## Contributing

To add new stress tests:

1. Add test function to appropriate file
2. Update `run_*_tests()` to call it
3. Document in this README
4. Update `STRESS_TEST_REPORT.md` with findings

## Resources

- [Chaos Engineering Principles](https://principlesofchaos.org/)
- [Linux Kernel Testing](https://www.kernel.org/doc/html/latest/dev-tools/testing-overview.html)
- [Syzkaller OS Fuzzer](https://github.com/google/syzkaller)
- [AFL++ Fuzzer](https://aflplus.plus/)

## License

Part of AutomationOS project.

## Contact

Issues? Open a GitHub issue or check the main project README.

---

**Last Updated:** 2026-05-26  
**Version:** 1.0  
**Status:** Active Development
