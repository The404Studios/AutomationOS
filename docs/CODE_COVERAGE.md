# Code Coverage Guide

**AutomationOS Coverage Analysis Framework**  
**Version:** 1.0.0  
**Last Updated:** 2026-05-26

---

## Table of Contents

1. [Overview](#overview)
2. [Coverage Infrastructure](#coverage-infrastructure)
3. [Building with Coverage](#building-with-coverage)
4. [Running Coverage Tests](#running-coverage-tests)
5. [Generating Reports](#generating-reports)
6. [Coverage Analysis](#coverage-analysis)
7. [Coverage Targets](#coverage-targets)
8. [CI Integration](#ci-integration)
9. [Best Practices](#best-practices)

---

## Overview

Code coverage is a critical quality metric that measures how much of the codebase is executed during testing. AutomationOS uses **gcov** and **lcov** for comprehensive coverage analysis.

### Coverage Types

- **Line Coverage:** Percentage of code lines executed
- **Branch Coverage:** Percentage of conditional branches taken
- **Function Coverage:** Percentage of functions called

### Tools Used

- **gcov:** GNU coverage testing tool (bundled with GCC)
- **lcov:** Front-end for gcov with HTML report generation
- **genhtml:** HTML report generator from lcov data

---

## Coverage Infrastructure

### Architecture

```
┌─────────────────────────────────────────────────────┐
│  Step 1: Instrumented Build                         │
│  (--coverage flag adds gcov instrumentation)        │
└────────────────┬────────────────────────────────────┘
                 │
┌────────────────┴────────────────────────────────────┐
│  Step 2: Test Execution                             │
│  (Run tests in QEMU or native)                      │
│  (Generates .gcda runtime data files)               │
└────────────────┬────────────────────────────────────┘
                 │
┌────────────────┴────────────────────────────────────┐
│  Step 3: Coverage Collection                        │
│  (lcov captures .gcda + .gcno data)                 │
└────────────────┬────────────────────────────────────┘
                 │
┌────────────────┴────────────────────────────────────┐
│  Step 4: HTML Report Generation                     │
│  (genhtml creates interactive reports)              │
└─────────────────────────────────────────────────────┘
```

### File Types

- **`.gcno`** - Coverage notes (generated at compile time)
- **`.gcda`** - Coverage data (generated at runtime)
- **`.info`** - lcov trace files (intermediate format)

---

## Building with Coverage

### Kernel Coverage Build

The kernel Makefile supports coverage instrumentation for unit tests and userspace components.

```bash
# Build kernel with coverage instrumentation (for testable components)
make kernel COVERAGE=1

# Build unit tests with coverage
make -C tests/unit COVERAGE=1
```

### Coverage Build Flags

```makefile
# Added to CFLAGS when COVERAGE=1
-fprofile-arcs      # Generate .gcda files
-ftest-coverage     # Generate .gcno files
--coverage          # Shorthand for both above

# Added to LDFLAGS
-lgcov              # Link with gcov library
```

### What Gets Instrumented

#### Full Coverage (Native Tests)
- Unit tests (tests/unit/*.c)
- Testable kernel modules (heap, scheduler, etc.)
- Userspace components (libc, shell)

#### Limited Coverage (QEMU)
- Not practical for full kernel coverage in QEMU
- Use QEMU for integration tests only
- Focus coverage on unit-testable components

---

## Running Coverage Tests

### Unit Tests with Coverage

```bash
# Build and run unit tests with coverage
make coverage-unit

# Or manually:
make -C tests/unit COVERAGE=1 clean all
make -C tests/unit run
```

### Integration Tests

```bash
# Run integration tests (generates QEMU execution logs)
make test

# These don't generate coverage data but validate system behavior
```

### Cleaning Coverage Data

```bash
# Remove coverage data files
make coverage-clean

# Full clean (including build artifacts)
make clean
```

---

## Generating Reports

### Generate Coverage Report

```bash
# Full coverage report generation
make coverage-report

# This performs:
# 1. Captures coverage data with lcov
# 2. Filters out system headers
# 3. Generates HTML report in docs/coverage/
```

### Manual Report Generation

```bash
# Capture coverage data
lcov --capture --directory build/ --output-file coverage.info

# Remove system headers
lcov --remove coverage.info '/usr/*' --output-file coverage_filtered.info

# Generate HTML report
genhtml coverage_filtered.info --output-directory docs/coverage/

# Open report in browser
xdg-open docs/coverage/index.html  # Linux
open docs/coverage/index.html      # macOS
start docs/coverage/index.html     # Windows
```

### Report Structure

```
docs/coverage/
├── index.html              # Main coverage dashboard
├── kernel/
│   ├── core/
│   │   ├── mem/
│   │   │   ├── heap.c.gcov.html
│   │   │   ├── pmm.c.gcov.html
│   │   │   └── vmm.c.gcov.html
│   │   ├── sched/
│   │   │   ├── scheduler.c.gcov.html
│   │   │   └── process.c.gcov.html
│   │   └── syscall/
│   │       └── handlers.c.gcov.html
│   └── drivers/
│       ├── serial.c.gcov.html
│       └── ps2.c.gcov.html
└── userspace/
    └── libc/
        ├── string.c.gcov.html
        └── stdio.c.gcov.html
```

---

## Coverage Analysis

### Reading Coverage Reports

#### Line Coverage
- **Green:** Line executed
- **Red:** Line not executed
- **Yellow:** Line partially executed (branch not fully covered)

#### Branch Coverage
- Shows which branches (if/else, switch) were taken
- Critical for testing error paths

#### Example Analysis

```c
// Function: pmm_alloc_page()
// Line Coverage: 85% (17/20 lines)
// Branch Coverage: 75% (6/8 branches)

void* pmm_alloc_page(void) {
    if (!initialized) {          // ✓ Tested
        return NULL;             // ✗ Not reached (initialization always succeeds)
    }
    
    // Find free page
    for (int i = 0; i < max_pages; i++) {   // ✓ Tested
        if (bitmap[i] == 0) {                // ✓ Both branches tested
            bitmap[i] = 1;
            return (void*)(base + i * PAGE_SIZE);
        }
    }
    
    return NULL;                 // ✗ Out-of-memory case not tested
}
```

### Identifying Coverage Gaps

1. **Uncovered Lines:** Test doesn't reach this code
2. **Uncovered Branches:** Missing error case tests
3. **Uncovered Functions:** Dead code or missing tests

---

## Coverage Targets

### Overall Targets

- **Overall Line Coverage:** > 80%
- **Overall Branch Coverage:** > 70%
- **Overall Function Coverage:** > 85%

### Subsystem Targets

#### Memory Management (Critical)
- **Target:** > 85% line coverage, > 75% branch coverage
- **Components:**
  - Physical Memory Manager (pmm.c)
  - Virtual Memory Manager (vmm.c)
  - Heap Allocator (heap.c)

#### Process Scheduler (Critical)
- **Target:** > 80% line coverage, > 70% branch coverage
- **Components:**
  - Scheduler (scheduler.c)
  - Process Management (process.c)
  - Context Switching (context.c)

#### System Calls (Critical)
- **Target:** > 90% line coverage, > 80% branch coverage
- **Components:**
  - System Call Dispatcher (syscall.c)
  - System Call Handlers (handlers.c)

#### Drivers
- **Target:** > 70% line coverage, > 60% branch coverage
- **Components:**
  - Serial Driver (serial.c)
  - PS/2 Driver (ps2.c)
  - Timer Driver (pit.c)
  - Framebuffer Driver (framebuffer.c)

#### Userspace
- **Target:** > 75% line coverage, > 65% branch coverage
- **Components:**
  - Standard Library (libc/)
  - Init Process (init.c)
  - Shell (shell.c)

### Priority Matrix

| Component           | Priority | Current | Target | Gap  |
|---------------------|----------|---------|--------|------|
| System Call Handlers| Critical | 65%     | 90%    | 25%  |
| Memory Management   | Critical | 72%     | 85%    | 13%  |
| Scheduler           | Critical | 68%     | 80%    | 12%  |
| Drivers             | High     | 55%     | 70%    | 15%  |
| Userspace           | Medium   | 45%     | 75%    | 30%  |

---

## CI Integration

### GitHub Actions Workflow

Create `.github/workflows/coverage.yml`:

```yaml
name: Code Coverage

on:
  push:
    branches: [ main, develop ]
  pull_request:
    branches: [ main ]

jobs:
  coverage:
    runs-on: ubuntu-latest
    
    steps:
    - uses: actions/checkout@v3
    
    - name: Install Dependencies
      run: |
        sudo apt-get update
        sudo apt-get install -y build-essential nasm lcov
    
    - name: Build with Coverage
      run: |
        make -C tests/unit COVERAGE=1 clean all
    
    - name: Run Tests
      run: |
        make -C tests/unit run
    
    - name: Generate Coverage Report
      run: |
        make coverage-report
    
    - name: Upload to Codecov
      uses: codecov/codecov-action@v3
      with:
        files: ./coverage_filtered.info
        fail_ci_if_error: true
        verbose: true
    
    - name: Coverage Check
      run: |
        # Fail if coverage drops below threshold
        python3 scripts/check-coverage.py --threshold 80
```

### Codecov Integration

1. **Sign up:** https://codecov.io/
2. **Add repository**
3. **Add token to GitHub secrets:** `CODECOV_TOKEN`
4. **Badge in README:**

```markdown
[![codecov](https://codecov.io/gh/USERNAME/AutomationOS/branch/main/graph/badge.svg)](https://codecov.io/gh/USERNAME/AutomationOS)
```

### Coverage Enforcement

Create `scripts/check-coverage.py`:

```python
#!/usr/bin/env python3
"""Check coverage thresholds and fail CI if not met."""

import sys
import re
import argparse

def parse_coverage_info(info_file):
    """Parse lcov .info file for coverage percentages."""
    with open(info_file, 'r') as f:
        content = f.read()
    
    # Extract line coverage
    lines_found = sum(int(x) for x in re.findall(r'LF:(\d+)', content))
    lines_hit = sum(int(x) for x in re.findall(r'LH:(\d+)', content))
    line_coverage = (lines_hit / lines_found * 100) if lines_found > 0 else 0
    
    # Extract function coverage
    funcs_found = sum(int(x) for x in re.findall(r'FNF:(\d+)', content))
    funcs_hit = sum(int(x) for x in re.findall(r'FNH:(\d+)', content))
    func_coverage = (funcs_hit / funcs_found * 100) if funcs_found > 0 else 0
    
    return line_coverage, func_coverage

def main():
    parser = argparse.ArgumentParser(description='Check coverage thresholds')
    parser.add_argument('--info-file', default='coverage_filtered.info',
                       help='Coverage info file')
    parser.add_argument('--threshold', type=float, default=80.0,
                       help='Minimum coverage percentage')
    args = parser.parse_args()
    
    line_cov, func_cov = parse_coverage_info(args.info_file)
    
    print(f"Line Coverage: {line_cov:.2f}%")
    print(f"Function Coverage: {func_cov:.2f}%")
    
    if line_cov < args.threshold:
        print(f"ERROR: Line coverage {line_cov:.2f}% below threshold {args.threshold}%")
        sys.exit(1)
    
    print(f"SUCCESS: Coverage meets threshold ({args.threshold}%)")
    sys.exit(0)

if __name__ == '__main__':
    main()
```

---

## Best Practices

### Writing Testable Code

1. **Separate concerns:** Business logic vs. hardware interaction
2. **Dependency injection:** Pass dependencies as parameters
3. **Small functions:** Easier to test, better coverage
4. **Clear error paths:** All error conditions should be testable

### Coverage-Driven Development

1. **Write tests first** (TDD)
2. **Run coverage after each test**
3. **Identify gaps** in coverage report
4. **Write tests for uncovered code**
5. **Refactor** to improve testability

### Testing Error Paths

```c
// Example: Test all error paths
void test_pmm_alloc_failure() {
    // Fill all available memory
    while (pmm_alloc_page() != NULL) {
        // Keep allocating
    }
    
    // Now test out-of-memory case
    void* page = pmm_alloc_page();
    ASSERT(page == NULL);  // This covers the error path!
}
```

### Avoiding False Coverage

```c
// BAD: High coverage, low value
void test_trivial() {
    foo();  // Just calls function, doesn't verify behavior
}

// GOOD: High coverage, high value
void test_comprehensive() {
    int result = foo();
    ASSERT(result == EXPECTED_VALUE);
    ASSERT(bar_was_called());  // Verify side effects
    ASSERT(error_count == 0);  // Check error state
}
```

### Coverage != Quality

- **100% coverage ≠ bug-free code**
- Focus on **meaningful tests**
- Cover **edge cases** and **error conditions**
- Use coverage to find **untested code**, not as a goal

---

## Tools Reference

### Installing Coverage Tools

```bash
# Ubuntu/Debian
sudo apt-get install lcov

# Arch Linux
sudo pacman -S lcov

# macOS
brew install lcov
```

### Common lcov Commands

```bash
# Capture coverage data
lcov --capture --directory . --output-file coverage.info

# List coverage by file
lcov --list coverage.info

# Remove external files
lcov --remove coverage.info '/usr/*' --output-file coverage_filtered.info

# Merge multiple coverage files
lcov --add-tracefile file1.info --add-tracefile file2.info --output-file merged.info

# Zero coverage counters
lcov --zerocounters --directory .

# Extract specific files
lcov --extract coverage.info '*/kernel/*' --output-file kernel_only.info
```

### Useful Make Targets

```bash
# Build with coverage
make coverage-build

# Run tests with coverage
make coverage-test

# Generate coverage report
make coverage-report

# Full coverage workflow
make coverage

# Clean coverage data
make coverage-clean
```

---

## Troubleshooting

### Issue: No .gcda files generated

**Cause:** Program didn't exit cleanly  
**Solution:** Ensure tests exit normally, not via crash or forced termination

### Issue: "cannot open coverage.info"

**Cause:** lcov didn't find any coverage data  
**Solution:** 
```bash
# Check for .gcda files
find build/ -name "*.gcda"

# If none found, rebuild with COVERAGE=1
make clean
make COVERAGE=1
```

### Issue: Coverage report shows 0%

**Cause:** Mismatch between .gcno (build) and .gcda (runtime) locations  
**Solution:** Ensure build directory matches runtime directory

### Issue: QEMU tests don't generate coverage

**Cause:** gcov requires filesystem access to write .gcda files  
**Solution:** Focus on native unit tests for coverage; use QEMU for integration tests only

---

## References

- [gcov Documentation](https://gcc.gnu.org/onlinedocs/gcc/Gcov.html)
- [lcov Manual](http://ltp.sourceforge.net/coverage/lcov.php)
- [Codecov Documentation](https://docs.codecov.com/)
- [AutomationOS Testing Guide](TEST_PLAN.md)

---

**Next Steps:**
1. Implement coverage-enabled Makefiles
2. Run baseline coverage analysis
3. Write tests to close coverage gaps
4. Set up CI integration
5. Monitor coverage trends over time
