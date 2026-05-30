# KTest Architecture

## System Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                     AutomationOS Kernel                         │
│                                                                 │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │                   Kernel Main (kernel.c)                   │ │
│  │                                                             │ │
│  │  1. kernel_tests_init()    ← Initialize test framework    │ │
│  │  2. pmm_init()             ← Initialize PMM                │ │
│  │  3. kernel_tests_run_phase(EARLY) ← Run PMM tests        │ │
│  │  4. vmm_init()             ← Initialize VMM                │ │
│  │  5. heap_init()            ← Initialize heap               │ │
│  │  6. kernel_tests_run_phase(MIDDLE) ← Run heap tests      │ │
│  │  7. syscall_init()         ← Initialize syscalls           │ │
│  │  8. kernel_tests_run_phase(LATE) ← Run syscall tests     │ │
│  │  9. kernel_tests_export_results() ← Print summary         │ │
│  └───────────────────────────────────────────────────────────┘ │
│                              ↓                                   │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │              KTest Framework (ktest.c)                     │ │
│  │                                                             │ │
│  │  • Test Registration                                        │ │
│  │  • Test Discovery                                           │ │
│  │  • Test Execution                                           │ │
│  │  • Assertion Handling                                       │ │
│  │  • Statistics Tracking                                      │ │
│  │  • Result Reporting                                         │ │
│  └───────────────────────────────────────────────────────────┘ │
│                              ↓                                   │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │                    Test Suites                             │ │
│  │                                                             │ │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐ │ │
│  │  │   PMM    │  │   VMM    │  │   Heap   │  │  Sched   │ │ │
│  │  │ 12 tests │  │ 11 tests │  │ 14 tests │  │ 15 tests │ │ │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────┘ │ │
│  │  ┌──────────┐  ┌──────────┐                              │ │
│  │  │ Syscall  │  │  String  │                              │ │
│  │  │ 23 tests │  │ 30 tests │                              │ │
│  │  └──────────┘  └──────────┘                              │ │
│  └───────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
                             ↓
                    Serial Output (QEMU)
                             ↓
              ┌──────────────────────────────┐
              │  parse_test_results.py       │
              └──────────────────────────────┘
                    ↓          ↓         ↓
              ┌────────┐  ┌────────┐  ┌────────┐
              │  JUnit │  │  JSON  │  │  HTML  │
              │   XML  │  │ Report │  │ Report │
              └────────┘  └────────┘  └────────┘
```

## Component Architecture

### 1. Test Framework Core (ktest.c)

```
┌────────────────────────────────────────────────────────┐
│              ktest_context_t (Global State)            │
├────────────────────────────────────────────────────────┤
│  • suites: ktest_suite_t*  ← Linked list of suites    │
│  • stats: ktest_stats_t    ← Test statistics          │
│  • enabled: bool           ← Framework enabled         │
│  • verbose: bool           ← Verbose mode              │
│  • filter: const char*     ← Test filter pattern       │
│  • current_test: ...       ← Currently running test    │
│  • expect_panic: bool      ← Death test flag           │
└────────────────────────────────────────────────────────┘
                             │
                             ↓
                    ┌────────────────┐
                    │  Test Suites   │
                    └────────────────┘
                             │
                ┌────────────┴────────────┐
                ↓                         ↓
        ┌──────────────┐        ┌──────────────┐
        │ Suite 1      │        │ Suite 2      │
        ├──────────────┤        ├──────────────┤
        │ • name       │        │ • name       │
        │ • setup()    │        │ • setup()    │
        │ • teardown() │        │ • teardown() │
        │ • fixture    │        │ • fixture    │
        │ • test_cases │        │ • test_cases │
        │ • next ──────┼────────┤ • next       │
        └──────────────┘        └──────────────┘
                │
                ↓
        ┌──────────────┐
        │  Test Case   │
        ├──────────────┤
        │ • name       │
        │ • test_fn()  │
        │ • result     │
        │ • message    │
        │ • file       │
        │ • line       │
        │ • next       │
        └──────────────┘
```

### 2. Test Execution Flow

```
Start
  │
  ↓
ktest_init()
  │
  ├─► Initialize global context
  ├─► Reset statistics
  ├─► Print banner
  └─► Set configuration
  │
  ↓
ktest_run_all()
  │
  ├─► Start timer
  ├─► For each suite:
  │    │
  │    ├─► Print suite header
  │    ├─► For each test case:
  │    │    │
  │    │    ├─► run_test_case()
  │    │    │    │
  │    │    │    ├─► Allocate fixture (if needed)
  │    │    │    ├─► Call setup() function
  │    │    │    ├─► Start timer
  │    │    │    ├─► Execute test_fn()
  │    │    │    │    │
  │    │    │    │    ├─► If assertion fails:
  │    │    │    │    │    ├─► __ktest_assert_failed()
  │    │    │    │    │    ├─► Set result = FAILURE
  │    │    │    │    │    └─► Return early
  │    │    │    │    │
  │    │    │    │    └─► If completes: result = SUCCESS
  │    │    │    │
  │    │    │    ├─► Stop timer
  │    │    │    ├─► Call teardown() function
  │    │    │    ├─► Free fixture
  │    │    │    └─► Report result (color-coded)
  │    │    │
  │    │    └─► Update statistics
  │    │
  │    └─► Print suite summary
  │
  ├─► Stop timer
  └─► Print overall summary
  │
  ↓
End
```

### 3. Assertion Flow

```
KTEST_ASSERT_EQ(a, b)
  │
  ↓
Macro expansion:
  if ((a) != (b)) {
    __ktest_assert_failed(...)
    return;
  }
  │
  ↓
__ktest_assert_failed()
  │
  ├─► Get current test context
  ├─► Set test result = FAILURE
  ├─► Format failure message:
  │    "Assertion failed: a == b"
  │    "Expected 42, got 99"
  └─► Store in current_test->failure_msg
  │
  ↓
Return from test function
  │
  ↓
Test marked as FAILED
```

### 4. Fixture Management

```
Test with Fixture
  │
  ↓
KTEST_SUITE_WITH_FIXTURE(name, type, setup, teardown)
  │
  ├─► Create suite structure
  ├─► Set fixture_size = sizeof(type)
  ├─► Set setup function pointer
  └─► Set teardown function pointer
  │
  ↓
Run test with fixture
  │
  ├─► Allocate fixture memory
  │    (static buffer or heap)
  ├─► Call setup(fixture)
  │    User initializes fixture
  ├─► Call test_fn(fixture)
  │    Test uses fixture data
  ├─► Call teardown(fixture)
  │    User cleans up fixture
  └─► Free fixture memory
```

### 5. Test Registration (Auto-Discovery)

```
Test Definition:
  KTEST_CASE(suite, name) { ... }
  │
  ↓
Macro expansion:
  1. Define test function
  2. Create static test_case structure
  3. Create __attribute__((constructor)) function
  │
  ↓
At kernel startup (before main):
  │
  ├─► Constructor functions run
  ├─► Each calls ktest_register_case()
  ├─► Tests added to suite's linked list
  └─► All tests discovered automatically
  │
  ↓
In kernel_main():
  │
  ├─► ktest_init() - framework ready
  └─► ktest_run_all() - runs all discovered tests
```

## Data Structures

### Test Suite
```c
typedef struct ktest_suite {
    const char* name;              // Suite name (e.g., "pmm")
    ktest_setup_fn setup;          // Setup function pointer
    ktest_teardown_fn teardown;    // Teardown function pointer
    void* fixture;                 // Fixture data (runtime allocated)
    size_t fixture_size;           // Size of fixture type
    ktest_case_t* test_cases;      // Linked list of test cases
    struct ktest_suite* next;      // Next suite in list
    bool enabled;                  // Suite enabled flag
};
```

### Test Case
```c
typedef struct ktest_case {
    const char* name;              // Test name (e.g., "alloc_returns_non_null")
    ktest_case_fn test_fn;         // Test function pointer
    ktest_result_t result;         // Result (SUCCESS/FAILURE/SKIPPED)
    const char* failure_msg;       // Failure message (if failed)
    const char* file;              // Source file (__FILE__)
    int line;                      // Source line (__LINE__)
    struct ktest_case* next;       // Next test case in suite
};
```

### Test Statistics
```c
typedef struct {
    uint32_t total;                // Total tests run
    uint32_t passed;               // Tests passed
    uint32_t failed;               // Tests failed
    uint32_t skipped;              // Tests skipped
    uint64_t start_time;           // Start timestamp (cycles)
    uint64_t end_time;             // End timestamp (cycles)
} ktest_stats_t;
```

## Integration Points

### 1. Kernel Boot Integration

```
kernel_main()
  │
  ├─► Phase 1: Early Init
  │   ├─► serial_init()
  │   ├─► kernel_tests_init() ◄─── Initialize test framework
  │   └─► parse_boot_params()  ◄─── Parse ktest= parameter
  │
  ├─► Phase 2: Memory Management
  │   ├─► pmm_init()
  │   ├─► vmm_init()
  │   └─► kernel_tests_run_phase(EARLY) ◄─── Test PMM/VMM
  │
  ├─► Phase 3: Core Subsystems
  │   ├─► heap_init()
  │   ├─► sched_init()
  │   └─► kernel_tests_run_phase(MIDDLE) ◄─── Test heap/sched
  │
  ├─► Phase 4: High-Level Features
  │   ├─► syscall_init()
  │   ├─► fs_init()
  │   └─► kernel_tests_run_phase(LATE) ◄─── Test syscalls/fs
  │
  └─► Phase 5: Complete
      └─► kernel_tests_export_results() ◄─── Print summary
```

### 2. Panic Handler Integration

```
kernel_panic()
  │
  ├─► Disable interrupts
  ├─► Print panic message
  ├─► if (kernel_tests_enabled()) {
  │      kernel_tests_on_panic(); ◄─── Report test context
  │   }
  ├─► Print stack trace
  └─► Halt
```

## Output Processing Pipeline

```
Kernel Serial Output
         │
         ↓
  ┌──────────────┐
  │ QEMU stdout  │
  └──────────────┘
         │
         ↓ (redirect to file)
  ┌──────────────┐
  │ test_output  │
  │    .log      │
  └──────────────┘
         │
         ↓
  ┌────────────────────────┐
  │ parse_test_results.py  │
  │                        │
  │ • Parse [RUN], [OK],   │
  │   [FAIL] lines         │
  │ • Extract statistics   │
  │ • Group by suite       │
  └────────────────────────┘
         │
    ┌────┴────┬─────────┐
    ↓         ↓         ↓
┌────────┐ ┌────────┐ ┌────────┐
│ JUnit  │ │  JSON  │ │  HTML  │
│  XML   │ │ Report │ │ Report │
└────────┘ └────────┘ └────────┘
    ↓
┌────────────┐
│  CI/CD     │
│ (Jenkins,  │
│  GitHub    │
│  Actions)  │
└────────────┘
```

## Test Lifecycle State Machine

```
                ┌──────────────┐
                │  REGISTERED  │ (test defined, constructor run)
                └──────────────┘
                       │
                       ↓ (ktest_run_all called)
                ┌──────────────┐
                │   PENDING    │ (waiting to run)
                └──────────────┘
                       │
                       ↓ (run_test_case called)
                ┌──────────────┐
                │   RUNNING    │ (test executing)
                └──────────────┘
                       │
         ┌─────────────┼─────────────┐
         ↓             ↓             ↓
  ┌──────────┐  ┌──────────┐  ┌──────────┐
  │  PASSED  │  │  FAILED  │  │ SKIPPED  │
  └──────────┘  └──────────┘  └──────────┘
         │             │             │
         └─────────────┴─────────────┘
                       ↓
                ┌──────────────┐
                │  COMPLETED   │ (stats updated)
                └──────────────┘
```

## Memory Layout

```
Kernel Memory Space
┌────────────────────────────────────┐
│  Kernel Code (.text)               │
│  ┌──────────────────────────────┐  │
│  │ ktest.c functions            │  │
│  │ test_*.c functions           │  │
│  └──────────────────────────────┘  │
├────────────────────────────────────┤
│  Kernel Data (.data, .bss)         │
│  ┌──────────────────────────────┐  │
│  │ g_test_ctx (global context)  │  │
│  │ Test suite structures        │  │
│  │ Test case structures         │  │
│  └──────────────────────────────┘  │
├────────────────────────────────────┤
│  Kernel Heap                       │
│  ┌──────────────────────────────┐  │
│  │ Fixture allocations          │  │
│  │ Test temporary data          │  │
│  └──────────────────────────────┘  │
├────────────────────────────────────┤
│  Stack                             │
│  ┌──────────────────────────────┐  │
│  │ Test function frames         │  │
│  │ Local variables              │  │
│  └──────────────────────────────┘  │
└────────────────────────────────────┘
```

## Thread Safety Considerations

Currently **single-threaded** - all tests run sequentially in kernel context.

Future multi-threaded architecture:
```
┌───────────────────────────────────────────┐
│         Test Coordinator Thread           │
├───────────────────────────────────────────┤
│  • Manages test queue                     │
│  • Spawns worker threads                  │
│  • Collects results                       │
│  • Updates statistics (with lock)         │
└───────────────────────────────────────────┘
              │
    ┌─────────┴─────────┐
    ↓                   ↓
┌─────────┐        ┌─────────┐
│ Worker  │        │ Worker  │
│ Thread  │        │ Thread  │
│   #1    │        │   #2    │
├─────────┤        ├─────────┤
│ Run     │        │ Run     │
│ Suite A │        │ Suite B │
└─────────┘        └─────────┘
```

## Extensibility Points

1. **New Test Suites**: Add file in `tests/`, define suite, write cases
2. **Custom Assertions**: Add macros in `ktest.h`
3. **New Report Formats**: Extend `parse_test_results.py`
4. **Test Phases**: Modify `kernel_tests_run_phase()`
5. **Mock Functions**: Implement in `ktest_mock.c`
6. **Coverage Tools**: Add compiler flags, gcov integration

## Performance Profile

```
Test Execution Time Breakdown:
┌────────────────────────────────┐
│ Framework Overhead   │ ~5%     │
│ Test Setup/Teardown  │ ~15%    │
│ Actual Test Code     │ ~75%    │
│ Result Reporting     │ ~5%     │
└────────────────────────────────┘

Per-Test Average: ~50-100ms
  - Setup: ~10ms
  - Execution: ~30-80ms
  - Teardown: ~5ms
  - Reporting: ~5ms
```

This architecture provides a solid, extensible foundation for comprehensive kernel testing!
