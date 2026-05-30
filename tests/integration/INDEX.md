# AutomationOS Integration Test Suite - File Index

## Overview

This directory contains the **Expanded Integration Test Suite** with **110+ integration tests** covering all major subsystems.

**Expansion Achievement**: 37 → 110 tests (197% increase)

## File Structure

```
tests/integration/
├── Test Implementation (8 files, ~3,200 LOC)
│   ├── test_boot_expanded.c              (10 tests)
│   ├── test_application_lifecycle.c      (15 tests)
│   ├── test_filesystem_integration.c     (15 tests)
│   ├── test_network_stack.c              (15 tests)
│   ├── test_graphics_stack.c             (10 tests)
│   ├── test_security_expanded.c          (20 tests)
│   ├── test_power_management.c           (10 tests)
│   └── integration_suite_expanded.c      (Master runner)
│
├── Original Tests (maintained)
│   ├── integration_suite.c               (15 tests - original)
│   ├── stress_test.c
│   └── regression_suite.c
│
├── Documentation (4 files, ~1,800 lines)
│   ├── EXPANDED_TEST_SUITE.md            (Comprehensive guide)
│   ├── EXPANSION_REPORT.md               (Detailed metrics)
│   ├── QUICK_REFERENCE.md                (Quick lookup)
│   └── INDEX.md                          (This file)
│
├── Build System
│   └── Makefile.expanded                 (Build and run tests)
│
└── Legacy Files (maintained)
    ├── test_boot.py                      (Python boot test)
    ├── test_*.py                         (Phase 2 Python tests)
    └── phase2/                           (Phase 2 test framework)
```

## Quick Navigation

### Want to...

**Run all tests?**
→ See [QUICK_REFERENCE.md](QUICK_REFERENCE.md#tldr)

**Understand test architecture?**
→ See [EXPANDED_TEST_SUITE.md](EXPANDED_TEST_SUITE.md#test-architecture)

**Check test status?**
→ See [EXPANSION_REPORT.md](EXPANSION_REPORT.md#test-execution-results)

**Add new tests?**
→ See [EXPANDED_TEST_SUITE.md](EXPANDED_TEST_SUITE.md#contributing)

**Debug failing tests?**
→ See [EXPANDED_TEST_SUITE.md](EXPANDED_TEST_SUITE.md#troubleshooting)

## Test Files by Category

### Category 1: Boot Sequence (10 tests) ✓
**File**: `test_boot_expanded.c`  
**Function**: `run_boot_integration_tests()`  
**Status**: Complete  
**Tests**: Cold boot, warm reboot, crash recovery, multi-user, safe mode, init ordering, driver init, service startup, shell readiness, performance

### Category 2: Application Lifecycle (15 tests) ✓
**File**: `test_application_lifecycle.c`  
**Function**: `run_application_lifecycle_tests()`  
**Status**: Complete  
**Tests**: Launch, suspend/resume, crash recovery, coordination, permissions, state, cleanup, update, IPC, termination, parent/child, orphans, daemons, interactive, background

### Category 3: File System Integration (15 tests) ⏳
**File**: `test_filesystem_integration.c`  
**Function**: `run_filesystem_integration_tests()`  
**Status**: Partial (Phase 3)  
**Tests**: VFS↔driver, cache coherency, concurrent access, large files, metadata, directories, mount, locks, symlinks, hard links, permissions, xattr, journaling, quotas, consistency

### Category 4: Network Stack (15 tests) ⏳
**File**: `test_network_stack.c`  
**Function**: `run_network_stack_integration_tests()`  
**Status**: Partial (Phase 3)  
**Tests**: Socket layer, TCP lifecycle, IP routing, Ethernet, drivers, concurrency, throughput, packet loss, firewall, NAT, dual-stack, UDP, ICMP, network NS, QoS

### Category 5: Graphics Stack (10 tests) ⏳
**File**: `test_graphics_stack.c`  
**Function**: `run_graphics_stack_integration_tests()`  
**Status**: Pending (Phase 4)  
**Tests**: GPU init, framebuffer, compositor, multi-monitor, resolution, VSync, HW accel, windows, input routing, performance

### Category 6: Security (20 tests) ✓
**File**: `test_security_expanded.c`  
**Function**: `run_security_expanded_integration_tests()`  
**Status**: Complete  
**Tests**: Cap lifecycle, cap inheritance, NS isolation, NS nesting, MAC enforcement, MAC transitions, sandbox filter, escape prevention, audit events, audit integrity, cap+MAC, NS+sandbox, rlimits, rlimit inheritance, secure boot, crypto, stress, multi-layer denial, performance, defense-in-depth

### Category 7: Power Management (10 tests) ⏳
**File**: `test_power_management.c`  
**Function**: `run_power_management_integration_tests()`  
**Status**: Pending (Phase 4)  
**Tests**: Suspend/resume, hibernation, power profiles, thermal, battery, device power, freq scaling, C-states, wake sources, power measurement

### Category 8: Core Subsystems (15 tests) ✓
**File**: `integration_suite.c` (original)  
**Function**: `run_integration_test_suite()`  
**Status**: Complete  
**Tests**: Boot flow, memory integration, memory pressure, leak detection, security stack, enforcement layers, audit, drivers, interrupts, syscalls, concurrent syscalls, multiprocess stress, security stress

## Master Test Runner

**File**: `integration_suite_expanded.c`

**Functions**:
```c
void run_expanded_integration_suite(void);   // All 110 tests
void run_integration_category(int n);        // Category n (1-8)
void run_integration_smoke_test(void);       // Quick validation
```

**Features**:
- Unified test execution
- Category selection
- Comprehensive reporting
- Environment info
- Memory leak detection
- Performance timing

## Build System

**File**: `Makefile.expanded`

**Key Targets**:
```bash
make                    # Build all tests
make test               # Run full suite
make category-N         # Run category N
make smoke              # Quick smoke test
make stats              # Show statistics
make clean              # Clean build
make help               # Show help
```

## Documentation Files

### 1. EXPANDED_TEST_SUITE.md
**Purpose**: Comprehensive test suite guide  
**Length**: ~1,200 lines  
**Sections**:
- Test architecture overview
- Detailed test descriptions
- Running instructions
- Test quality standards
- Performance targets
- Troubleshooting guide
- Contributing guidelines

### 2. EXPANSION_REPORT.md
**Purpose**: Detailed expansion metrics and status  
**Length**: ~500 lines  
**Sections**:
- Executive summary
- Test breakdown by category
- Integration coverage matrix
- Test execution results
- Performance impact
- Deliverables
- Next steps

### 3. QUICK_REFERENCE.md
**Purpose**: Quick lookup for developers  
**Length**: ~200 lines  
**Sections**:
- TL;DR commands
- File overview
- Common tasks
- Test quality checklist
- Performance targets
- Quick commands

### 4. INDEX.md
**Purpose**: File navigation (this document)  
**Length**: ~250 lines  
**Sections**:
- File structure
- Quick navigation
- Category breakdown
- File locations

## Test Statistics

```
Total Files Created:     11
├── Test Implementation: 8 files
├── Documentation:       4 files (including this)
└── Build System:        1 file

Total Lines of Code:     ~4,500 LOC
├── Test Code:          ~3,200 LOC
├── Documentation:      ~1,800 lines
└── Build Scripts:      ~150 lines

Total Tests:             110
├── Boot Sequence:      10 tests
├── Application:        15 tests
├── File System:        15 tests
├── Network Stack:      15 tests
├── Graphics Stack:     10 tests
├── Security:           20 tests
├── Power Management:   10 tests
└── Core Subsystems:    15 tests

Test Status:
├── Ready (Phase 2):    60 tests (55%)
├── Phase 3 Pending:    30 tests (27%)
└── Phase 4 Pending:    20 tests (18%)
```

## Integration Points Covered

```
Memory:    PMM ↔ VMM ↔ Heap ↔ Process
Security:  Cap ↔ NS ↔ MAC ↔ Sandbox ↔ Audit
Boot:      UEFI → Bootloader → Kernel → Drivers → Services
Syscall:   User → Dispatch → Security → Kernel → Return
FS:        VFS ↔ Block ↔ Driver (Phase 3)
Network:   Socket ↔ TCP ↔ IP ↔ Ethernet ↔ NIC (Phase 3)
Graphics:  GPU ↔ Compositor ↔ App (Phase 4)
Power:     ACPI ↔ PM ↔ Devices (Phase 4)
```

## Related Directories

- `tests/unit/` - Unit tests for individual functions
- `tests/bench/` - Performance benchmarks
- `tests/fuzz/` - Fuzzing tests
- `tests/drivers/` - Driver-specific tests
- `tests/integration/phase2/` - Phase 2 Python integration tests

## Version History

| Version | Date | Tests | Status |
|---------|------|-------|--------|
| 1.0 | 2026-05-20 | 37 | Original |
| 2.0 | 2026-05-26 | 110 | Expanded (197% increase) |

## Contact

For issues or questions:
1. Check documentation in this directory
2. Review test logs in `build/test-reports/`
3. Examine kernel serial output for errors

---

**Last Updated**: 2026-05-26  
**Maintainer**: Integration Test Team  
**Status**: ✓ Complete (Phase 2), ⏳ Partial (Phase 3-4)
