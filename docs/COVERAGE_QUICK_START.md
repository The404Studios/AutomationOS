# Code Coverage - Quick Start Guide

**For Developers:** Get started with code coverage in 5 minutes.

---

## TL;DR

```bash
# Generate coverage report
make coverage

# View report
xdg-open docs/coverage/index.html  # Linux/WSL
open docs/coverage/index.html      # macOS
start docs/coverage/index.html     # Windows
```

---

## Installation

### Ubuntu/Debian
```bash
sudo apt-get install lcov
```

### Arch Linux
```bash
sudo pacman -S lcov
```

### macOS
```bash
brew install lcov
```

---

## Daily Workflow

### 1. Write Code
```bash
vim kernel/core/syscall/handlers.c
```

### 2. Write Tests
```bash
vim tests/unit/test_syscall_handlers.c
```

### 3. Run Coverage
```bash
make coverage
```

### 4. View Report
```bash
xdg-open docs/coverage/index.html
```

### 5. Identify Gaps
Look for **red lines** in the HTML report - these are uncovered.

### 6. Add Tests
Write tests to cover the red lines.

### 7. Verify
```bash
make coverage
```

---

## Common Commands

```bash
# Full coverage workflow
make coverage

# Just build tests with coverage
make coverage-build

# Just run tests
make coverage-test

# Just generate report
make coverage-report

# Clean coverage data
make coverage-clean

# Run specific test
./tests/unit/test_vmm
```

---

## Understanding Reports

### Line Coverage
- **Green:** Line executed ✅
- **Red:** Line not executed ❌
- **Yellow:** Branch partially covered ⚠️

### Coverage Goals
- **System Calls:** 90%+
- **Memory:** 85%+
- **Scheduler:** 80%+
- **Drivers:** 70%+
- **Userspace:** 75%+
- **Overall:** 80%+

---

## Writing Good Tests

### ✅ DO
```c
TEST(read_with_invalid_fd) {
    char buf[100];
    int result = sys_read(999, buf, 100);
    ASSERT(result == -EBADF);  // Verify error code
}
```

### ❌ DON'T
```c
TEST(read_basic) {
    sys_read(0, buf, 100);  // No verification!
}
```

---

## Test Template

```c
#include <stdio.h>
#include <assert.h>

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT(condition) do { \
    tests_run++; \
    if (condition) { \
        tests_passed++; \
    } else { \
        tests_failed++; \
        fprintf(stderr, "[FAIL] %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
    } \
} while(0)

#define TEST(name) void test_##name()

#define RUN_TEST(name) do { \
    printf("Running test: %s... ", #name); \
    test_##name(); \
    printf("PASS\n"); \
} while(0)

TEST(example_test) {
    int result = my_function(42);
    ASSERT(result == 84);
}

int main() {
    RUN_TEST(example_test);
    
    printf("\nTests: %d, Passed: %d, Failed: %d\n",
           tests_run, tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
```

---

## CI Integration

Coverage runs automatically on:
- Every push to `main` or `develop`
- Every pull request
- Manual workflow dispatch

Reports are:
- Uploaded to Codecov
- Commented on PRs
- Available as artifacts

---

## Troubleshooting

### No coverage data?
```bash
make coverage-clean
make coverage
```

### Coverage report is empty?
```bash
# Ensure tests ran
make -C tests/unit run

# Check for .gcda files
find . -name "*.gcda"
```

### lcov not found?
```bash
# Install lcov (see Installation section above)
```

---

## Further Reading

- [CODE_COVERAGE.md](CODE_COVERAGE.md) - Complete guide
- [TEST_PLAN.md](TEST_PLAN.md) - Test strategy
- [docs/coverage/README.md](coverage/README.md) - Report docs

---

## Quick Reference

| Task | Command |
|------|---------|
| Generate coverage | `make coverage` |
| View report | `xdg-open docs/coverage/index.html` |
| Clean coverage data | `make coverage-clean` |
| Run tests | `make -C tests/unit run` |
| Check thresholds | `python3 scripts/check-coverage.py` |

---

**Questions?** Check the full documentation in `docs/CODE_COVERAGE.md`
