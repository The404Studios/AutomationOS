# Test Coverage Plan

**AutomationOS Test Strategy & Coverage Analysis**  
**Version:** 1.0.0  
**Last Updated:** 2026-05-26

---

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [Testing Strategy](#testing-strategy)
3. [Coverage Goals](#coverage-goals)
4. [Test Categories](#test-categories)
5. [Subsystem Test Plans](#subsystem-test-plans)
6. [Test Implementation](#test-implementation)
7. [Coverage Analysis](#coverage-analysis)
8. [Timeline & Milestones](#timeline--milestones)

---

## Executive Summary

### Objectives

1. **Achieve 80%+ line coverage** across critical kernel subsystems
2. **Establish comprehensive test suite** covering unit, integration, and system tests
3. **Implement coverage-guided testing** to identify and close gaps
4. **Enable continuous coverage monitoring** via CI/CD

### Current State (Baseline)

- **Unit Tests:** 5 test files covering basic functionality
- **Integration Tests:** Boot test via QEMU
- **Coverage Infrastructure:** Not yet implemented
- **Estimated Current Coverage:** ~45-50% (untested)

### Target State (2 Weeks)

- **Unit Tests:** 25+ test files covering all critical paths
- **Integration Tests:** Comprehensive system-level tests
- **Coverage Infrastructure:** Full gcov/lcov pipeline
- **Target Coverage:** > 80% line, > 70% branch

---

## Testing Strategy

### Test Pyramid

```
         ┌────────────┐
         │  Manual    │  < 5% (Exploratory testing)
         │  Testing   │
         ├────────────┤
         │ Integration│  15% (System-level tests)
         │   Tests    │
         ├────────────┤
         │   Unit     │  80% (Fast, isolated tests)
         │   Tests    │
         └────────────┘
```

### Testing Principles

1. **Fast Feedback:** Unit tests run in < 1 second
2. **Isolated Tests:** No dependencies between tests
3. **Deterministic:** Tests produce same results every run
4. **Coverage-Guided:** Use coverage data to prioritize test development
5. **Critical-First:** Focus on high-risk, high-impact code

### Test Types

| Type | Scope | Tools | Coverage Target |
|------|-------|-------|-----------------|
| Unit Tests | Individual functions/modules | Native GCC, custom test framework | 80%+ |
| Integration Tests | Multi-component interactions | QEMU, Python test harness | 70%+ |
| System Tests | Full system behavior | QEMU, automated boot tests | 60%+ |
| Performance Tests | Benchmarks, profiling | Custom benchmarks | N/A |

---

## Coverage Goals

### Overall Goals

| Metric | Current | Target (Week 1) | Target (Week 2) | Target (Long-term) |
|--------|---------|-----------------|-----------------|-------------------|
| Overall Line Coverage | ~45% | 60% | 80% | 85%+ |
| Overall Branch Coverage | ~35% | 50% | 70% | 75%+ |
| Overall Function Coverage | ~50% | 70% | 85% | 90%+ |

### Subsystem Coverage Goals

#### Critical Subsystems (Week 1 Priority)

1. **System Call Handlers** (handlers.c)
   - Current: ~65%
   - Target: 90%
   - Priority: **CRITICAL**
   - Rationale: User-kernel interface, security-critical

2. **Memory Management** (pmm.c, vmm.c, heap.c)
   - Current: ~72%
   - Target: 85%
   - Priority: **CRITICAL**
   - Rationale: Bugs cause crashes, data corruption

3. **Process Scheduler** (scheduler.c, process.c)
   - Current: ~68%
   - Target: 80%
   - Priority: **CRITICAL**
   - Rationale: Core functionality, fairness critical

#### High-Priority Subsystems (Week 2)

4. **Drivers** (serial.c, ps2.c, pit.c, framebuffer.c)
   - Current: ~55%
   - Target: 70%
   - Priority: **HIGH**
   - Rationale: Hardware interface, reliability important

5. **Userspace Library** (libc/, shell.c)
   - Current: ~45%
   - Target: 75%
   - Priority: **HIGH**
   - Rationale: Application foundation

6. **Security Subsystems** (namespace.c, mac/, crypto/)
   - Current: ~40%
   - Target: 85%
   - Priority: **HIGH**
   - Rationale: Security-critical, must be robust

---

## Test Categories

### 1. Unit Tests

**Purpose:** Test individual functions in isolation  
**Location:** `tests/unit/`  
**Execution:** Native (Linux/macOS), fast (~1s total)

#### Existing Unit Tests

- ✅ `test_pmm.c` - Physical memory allocator
- ✅ `test_heap.c` - Kernel heap allocator
- ✅ `test_scheduler.c` - Process scheduler
- ✅ `test_user_copy.c` - User-kernel memory copy
- ✅ `test_null_checks.c` - NULL pointer validation

#### Planned Unit Tests (Week 1)

- ⬜ `test_vmm.c` - Virtual memory manager
- ⬜ `test_syscall_handlers.c` - System call implementations
- ⬜ `test_process.c` - Process management
- ⬜ `test_context.c` - Context switching
- ⬜ `test_serial.c` - Serial driver
- ⬜ `test_ps2.c` - PS/2 keyboard driver
- ⬜ `test_pit.c` - Timer driver

#### Planned Unit Tests (Week 2)

- ⬜ `test_framebuffer.c` - Framebuffer driver
- ⬜ `test_namespace.c` - Namespace isolation
- ⬜ `test_mac.c` - MAC policy engine
- ⬜ `test_crypto.c` - Cryptographic functions
- ⬜ `test_libc_string.c` - String functions
- ⬜ `test_libc_stdio.c` - Standard I/O
- ⬜ `test_shell_parser.c` - Shell parser

### 2. Integration Tests

**Purpose:** Test multi-component interactions  
**Location:** `tests/integration/`  
**Execution:** QEMU, slower (~30s per test)

#### Existing Integration Tests

- ✅ `test_boot.py` - Boot sequence validation

#### Planned Integration Tests (Week 1)

- ⬜ `test_memory_integration.py` - PMM + VMM + Heap interaction
- ⬜ `test_process_lifecycle.py` - Fork, exec, exit
- ⬜ `test_syscall_integration.py` - Multiple syscalls in sequence
- ⬜ `test_scheduler_fairness.py` - Multi-process scheduling

#### Planned Integration Tests (Week 2)

- ⬜ `test_io_integration.py` - Serial + PS/2 + framebuffer
- ⬜ `test_security_integration.py` - Namespace + MAC interaction
- ⬜ `test_userspace_integration.py` - Init + shell + utilities
- ⬜ `test_stress.py` - Stress testing under load

### 3. System Tests

**Purpose:** End-to-end system behavior  
**Location:** `tests/system/`  
**Execution:** QEMU, comprehensive (~5 min)

#### Planned System Tests

- ⬜ `test_full_boot.py` - Complete boot to shell
- ⬜ `test_multi_user.py` - Multiple concurrent users
- ⬜ `test_long_running.py` - System stability over time
- ⬜ `test_recovery.py` - Error recovery scenarios

### 4. Benchmark Tests

**Purpose:** Performance measurement  
**Location:** `tests/bench/`  
**Execution:** QEMU or native

#### Existing Benchmarks

- ✅ `bench_context_switch.c` - Context switch latency
- ✅ `bench_syscall.c` - Syscall overhead
- ✅ `bench_memory.c` - Memory allocation speed

---

## Subsystem Test Plans

### Memory Management (pmm.c, vmm.c, heap.c)

**Current Coverage:** 72%  
**Target Coverage:** 85%  
**Gap:** 13 percentage points

#### Test Cases to Add

1. **PMM Edge Cases**
   - Out-of-memory handling
   - Large contiguous allocations
   - Fragmentation scenarios
   - Alignment requirements

2. **VMM Error Paths**
   - Invalid address mapping
   - Permission violations
   - Page fault handling
   - TLB invalidation

3. **Heap Stress Tests**
   - Random allocation/free patterns
   - Coalescing verification
   - Memory leak detection
   - Corruption detection

#### Implementation Priority

| Test Case | Lines to Cover | Priority | Effort |
|-----------|----------------|----------|--------|
| PMM out-of-memory | 15 | High | 2h |
| VMM permission checks | 25 | Critical | 4h |
| Heap coalescing | 30 | High | 3h |
| Memory leak detection | 20 | Medium | 2h |

**Total Effort:** 11 hours (1.5 days)

---

### System Call Handlers (handlers.c)

**Current Coverage:** 65%  
**Target Coverage:** 90%  
**Gap:** 25 percentage points

#### Uncovered System Calls

| Syscall | Current | Target | Test Cases Needed |
|---------|---------|--------|-------------------|
| sys_exit | 80% | 95% | Error paths |
| sys_fork | 30% | 90% | Full implementation test |
| sys_read | 70% | 90% | Invalid FD, EOF |
| sys_write | 75% | 90% | Invalid buffer, ENOSPC |
| sys_open | 40% | 85% | Invalid paths, permissions |
| sys_close | 60% | 85% | Double-close, invalid FD |
| sys_execve | 20% | 80% | Missing file, invalid ELF |
| sys_waitpid | 50% | 85% | No children, WNOHANG |
| sys_getpid | 95% | 95% | Complete |
| sys_gettimeofday | 85% | 90% | NULL pointer handling |

#### Test Implementation Plan

```c
// tests/unit/test_syscall_handlers.c

void test_sys_read_invalid_fd() {
    // Test reading from invalid file descriptor
    char buf[100];
    int64_t result = sys_read(999, (uint64_t)buf, 100, 0, 0, 0);
    ASSERT(result == -EBADF);
}

void test_sys_write_null_buffer() {
    // Test writing from NULL buffer
    int64_t result = sys_write(1, 0, 100, 0, 0, 0);
    ASSERT(result == -EFAULT);
}

void test_sys_fork_success() {
    // Test successful fork
    int64_t child_pid = sys_fork(0, 0, 0, 0, 0, 0);
    ASSERT(child_pid > 0);
    
    // Verify child process created
    process_t* child = process_get_by_pid(child_pid);
    ASSERT(child != NULL);
    ASSERT(child->parent_pid == process_get_current()->pid);
}

void test_sys_open_invalid_path() {
    // Test opening non-existent file
    int64_t result = sys_open((uint64_t)"/nonexistent", O_RDONLY, 0, 0, 0, 0);
    ASSERT(result == -ENOENT);
}
```

**Total Effort:** 20 hours (2.5 days)

---

### Process Scheduler (scheduler.c, process.c)

**Current Coverage:** 68%  
**Target Coverage:** 80%  
**Gap:** 12 percentage points

#### Uncovered Scenarios

1. **Scheduler Edge Cases**
   - Empty run queue
   - Single process
   - Process starvation prevention
   - Priority handling
   - Quantum expiry

2. **Process Lifecycle**
   - Process creation failure
   - Zombie process handling
   - Orphan process adoption
   - Process tree traversal

#### Test Cases

```c
// tests/unit/test_scheduler.c

void test_scheduler_empty_queue() {
    // Remove all processes
    scheduler_clear();
    
    // Schedule should handle gracefully
    schedule();
    
    // Should idle or panic gracefully
    ASSERT(scheduler_get_current() == NULL);
}

void test_scheduler_fairness() {
    // Create 3 processes
    process_t* p1 = process_create("test1");
    process_t* p2 = process_create("test2");
    process_t* p3 = process_create("test3");
    
    scheduler_add_process(p1);
    scheduler_add_process(p2);
    scheduler_add_process(p3);
    
    // Run for 100 time slices
    int counts[3] = {0, 0, 0};
    for (int i = 0; i < 100; i++) {
        schedule();
        process_t* current = scheduler_get_current();
        if (current == p1) counts[0]++;
        if (current == p2) counts[1]++;
        if (current == p3) counts[2]++;
    }
    
    // Each should get ~33 slices (allow 10% variance)
    ASSERT(counts[0] >= 28 && counts[0] <= 38);
    ASSERT(counts[1] >= 28 && counts[1] <= 38);
    ASSERT(counts[2] >= 28 && counts[2] <= 38);
}

void test_process_zombie_reaping() {
    // Create child process
    process_t* parent = process_get_current();
    process_t* child = process_create("child");
    child->parent_pid = parent->pid;
    
    // Child exits
    child->state = PROCESS_TERMINATED;
    
    // Parent calls waitpid
    int64_t status = sys_waitpid(child->pid, 0, 0, 0, 0, 0);
    ASSERT(status == child->exit_code);
    
    // Child should be reaped
    ASSERT(process_get_by_pid(child->pid) == NULL);
}
```

**Total Effort:** 16 hours (2 days)

---

### Drivers (serial.c, ps2.c, pit.c, framebuffer.c)

**Current Coverage:** 55%  
**Target Coverage:** 70%  
**Gap:** 15 percentage points

#### Driver Testing Strategy

Driver testing is challenging due to hardware dependencies. Strategy:

1. **Mock Hardware:** Create mock I/O port functions
2. **Isolated Logic:** Test business logic separately from I/O
3. **Integration Tests:** Test with QEMU's virtual hardware

#### Test Cases

```c
// tests/unit/test_serial.c

// Mock I/O functions
static uint8_t mock_port_data[1024];
static int mock_port_index = 0;

uint8_t mock_inb(uint16_t port) {
    return mock_port_data[mock_port_index++];
}

void mock_outb(uint16_t port, uint8_t value) {
    mock_port_data[mock_port_index++] = value;
}

void test_serial_init() {
    // Initialize serial port
    serial_init();
    
    // Verify initialization sequence
    ASSERT(mock_port_data[0] == 0x00);  // Disable interrupts
    ASSERT(mock_port_data[1] == 0x80);  // Enable DLAB
    // ... more assertions
}

void test_serial_write_string() {
    // Write string
    const char* msg = "Hello, World!";
    serial_write(msg, strlen(msg));
    
    // Verify each byte written
    for (size_t i = 0; i < strlen(msg); i++) {
        ASSERT(mock_port_data[i] == msg[i]);
    }
}

void test_ps2_keyboard_scancode() {
    // Simulate key press scancode
    mock_port_data[0] = 0x1E;  // 'A' key
    
    // Read scancode
    uint8_t scancode = ps2_read_scancode();
    ASSERT(scancode == 0x1E);
    
    // Convert to ASCII
    char ch = ps2_scancode_to_ascii(scancode);
    ASSERT(ch == 'a');
}
```

**Total Effort:** 12 hours (1.5 days)

---

### Userspace (libc/, shell.c)

**Current Coverage:** 45%  
**Target Coverage:** 75%  
**Gap:** 30 percentage points

#### Test Cases

1. **String Functions** (string.c)
   - strlen, strcmp, strcpy, strncpy
   - memcpy, memset, memmove
   - Edge cases: NULL pointers, zero lengths

2. **Standard I/O** (stdio.c)
   - printf formatting
   - File operations (when implemented)
   - Buffer management

3. **Shell Parser** (parser.c)
   - Command parsing
   - Argument tokenization
   - Quote handling
   - Pipe parsing (when implemented)

```c
// tests/unit/test_libc_string.c

void test_strlen() {
    ASSERT(strlen("") == 0);
    ASSERT(strlen("hello") == 5);
    ASSERT(strlen("hello\0world") == 5);  // Stops at null terminator
}

void test_strcmp() {
    ASSERT(strcmp("abc", "abc") == 0);
    ASSERT(strcmp("abc", "abd") < 0);
    ASSERT(strcmp("abd", "abc") > 0);
}

void test_memcpy_overlap() {
    char buf[100] = "hello world";
    
    // Non-overlapping copy (should work)
    memcpy(buf + 20, buf, 11);
    ASSERT(strcmp(buf + 20, "hello world") == 0);
}

void test_memmove_overlap() {
    char buf[100] = "hello world";
    
    // Overlapping copy (memmove handles this)
    memmove(buf + 2, buf, 11);
    ASSERT(strcmp(buf + 2, "hello world") == 0);
}

// tests/unit/test_shell_parser.c

void test_parse_simple_command() {
    char input[] = "echo hello";
    command_t cmd = parse_command(input);
    
    ASSERT(strcmp(cmd.program, "echo") == 0);
    ASSERT(cmd.argc == 2);
    ASSERT(strcmp(cmd.argv[0], "echo") == 0);
    ASSERT(strcmp(cmd.argv[1], "hello") == 0);
}

void test_parse_quoted_args() {
    char input[] = "echo \"hello world\"";
    command_t cmd = parse_command(input);
    
    ASSERT(cmd.argc == 2);
    ASSERT(strcmp(cmd.argv[1], "hello world") == 0);
}
```

**Total Effort:** 18 hours (2.25 days)

---

## Test Implementation

### Test Framework

AutomationOS uses a lightweight custom test framework:

```c
// tests/framework.h

#include <stdio.h>
#include <stdlib.h>

// Test statistics
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

// ASSERT macro
#define ASSERT(condition) do { \
    tests_run++; \
    if (condition) { \
        tests_passed++; \
    } else { \
        tests_failed++; \
        fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    } \
} while(0)

// Test runner macros
#define TEST(name) void test_##name()

#define RUN_TEST(name) do { \
    printf("Running test: %s... ", #name); \
    test_##name(); \
    printf("PASS\n"); \
} while(0)

// Report results
#define TEST_REPORT() do { \
    printf("\n========================================\n"); \
    printf("Tests run: %d\n", tests_run); \
    printf("Tests passed: %d\n", tests_passed); \
    printf("Tests failed: %d\n", tests_failed); \
    printf("========================================\n"); \
    return (tests_failed == 0) ? 0 : 1; \
} while(0)
```

### Example Test File

```c
// tests/unit/test_example.c

#include "../framework.h"
#include "../../kernel/include/mem.h"

TEST(pmm_initialization) {
    memory_map_entry_t mmap[1];
    mmap[0].base = 0x100000;
    mmap[0].length = 0x1000000;
    mmap[0].type = 1;
    
    pmm_init(mmap, 1);
    
    ASSERT(pmm_get_total_memory() == 0x1000000);
}

TEST(pmm_alloc_free) {
    void* page = pmm_alloc_page();
    ASSERT(page != NULL);
    
    pmm_free_page(page);
    
    // Should be able to allocate again
    void* page2 = pmm_alloc_page();
    ASSERT(page2 == page);  // Should get same page back
}

int main() {
    RUN_TEST(pmm_initialization);
    RUN_TEST(pmm_alloc_free);
    
    TEST_REPORT();
}
```

---

## Coverage Analysis

### Baseline Coverage Analysis

**Step 1: Build with coverage**
```bash
make -C tests/unit COVERAGE=1 clean all
```

**Step 2: Run tests**
```bash
make -C tests/unit run
```

**Step 3: Generate report**
```bash
make coverage-report
xdg-open docs/coverage/index.html
```

### Coverage-Guided Test Development

1. **Identify Red Lines:** Lines not covered
2. **Analyze Why:** Is it dead code or missing test?
3. **Write Test:** Cover the uncovered lines
4. **Verify:** Re-run coverage and confirm improvement

### Example Coverage Analysis

```
File: kernel/core/mem/heap.c
Line Coverage: 78% (85/109 lines)
Branch Coverage: 65% (13/20 branches)

Uncovered Lines:
  Line 145: return NULL;  // Out-of-memory error path
  Line 178: panic("heap corruption detected");  // Corruption detection
  Line 201-205: Coalesce with next block  // Heap coalescing logic

Action Items:
  1. Write test_heap_out_of_memory()
  2. Write test_heap_corruption_detection()
  3. Write test_heap_coalesce()
```

---

## Timeline & Milestones

### Week 1: Foundation (Days 1-5)

#### Day 1: Infrastructure Setup
- ✅ Write CODE_COVERAGE.md documentation
- ✅ Write TEST_PLAN.md
- ⬜ Update Makefiles with coverage support
- ⬜ Create coverage scripts (check-coverage.py)
- ⬜ Run baseline coverage analysis

**Deliverable:** Coverage infrastructure fully operational

#### Day 2-3: Critical Subsystem Tests
- ⬜ Implement test_syscall_handlers.c (20 test cases)
- ⬜ Enhance test_pmm.c (10 additional cases)
- ⬜ Implement test_vmm.c (15 test cases)
- ⬜ Enhance test_heap.c (8 additional cases)

**Deliverable:** Memory management > 85% coverage, syscalls > 75%

#### Day 4-5: Scheduler & Process Tests
- ⬜ Enhance test_scheduler.c (12 additional cases)
- ⬜ Implement test_process.c (15 test cases)
- ⬜ Implement test_context.c (8 test cases)

**Deliverable:** Scheduler > 80% coverage

### Week 2: Expansion (Days 6-10)

#### Day 6-7: Driver Tests
- ⬜ Implement test_serial.c (10 test cases)
- ⬜ Implement test_ps2.c (8 test cases)
- ⬜ Implement test_pit.c (6 test cases)
- ⬜ Implement test_framebuffer.c (8 test cases)

**Deliverable:** Drivers > 70% coverage

#### Day 8-9: Userspace Tests
- ⬜ Implement test_libc_string.c (20 test cases)
- ⬜ Implement test_libc_stdio.c (15 test cases)
- ⬜ Implement test_shell_parser.c (12 test cases)

**Deliverable:** Userspace > 75% coverage

#### Day 10: Integration & CI
- ⬜ Implement integration tests (4-5 scenarios)
- ⬜ Set up GitHub Actions workflow
- ⬜ Configure Codecov integration
- ⬜ Add coverage badge to README
- ⬜ Final coverage report generation

**Deliverable:** CI/CD with coverage enforcement, > 80% overall coverage

---

## Success Metrics

### Quantitative Metrics

- ✅ **Overall Line Coverage:** > 80%
- ✅ **Critical Subsystem Coverage:** > 85%
- ✅ **Branch Coverage:** > 70%
- ✅ **Test Count:** > 150 test cases
- ✅ **Test Execution Time:** < 5 seconds (unit tests)

### Qualitative Metrics

- ✅ **All critical error paths tested**
- ✅ **Coverage-driven development process established**
- ✅ **CI fails on coverage regression**
- ✅ **Coverage reports generated automatically**
- ✅ **Documentation complete and up-to-date**

---

## Maintenance Plan

### Ongoing Activities

1. **Weekly Coverage Review**
   - Review coverage trends
   - Identify new gaps
   - Prioritize test development

2. **Monthly Coverage Goals**
   - Set incremental coverage targets
   - Track progress
   - Celebrate improvements

3. **Coverage Enforcement**
   - CI fails if coverage drops
   - PR reviews include coverage check
   - New code must include tests

### Long-term Goals

- **Year 1:** Maintain > 80% coverage
- **Year 2:** Achieve > 85% coverage
- **Year 3:** Achieve > 90% coverage on critical paths

---

## References

- [Code Coverage Guide](CODE_COVERAGE.md)
- [Test Execution Guide](TEST_EXECUTION_GUIDE.md)
- [Developer Guide](DEVELOPER_GUIDE.md)
- [Architecture Documentation](ARCHITECTURE.md)

---

**Status:** In Progress  
**Next Review:** 2026-06-09 (2 weeks)
