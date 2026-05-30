# Integration Test Suite - Quick Reference

## TL;DR

```bash
# Build all tests
make -f Makefile.expanded

# Run everything (110 tests, ~5 minutes)
make -f Makefile.expanded test

# Quick validation (< 30 seconds)
make -f Makefile.expanded smoke

# Run specific category
make -f Makefile.expanded category-1    # Boot tests
make -f Makefile.expanded category-6    # Security tests
```

## Test Files at a Glance

| File | Tests | What It Tests | Ready? |
|------|-------|---------------|--------|
| `test_boot_expanded.c` | 10 | Boot sequence, init order | ✓ Yes |
| `test_application_lifecycle.c` | 15 | App launch/suspend/IPC | ✓ Yes |
| `test_filesystem_integration.c` | 15 | VFS, file ops, mount | ⏳ Phase 3 |
| `test_network_stack.c` | 15 | Socket, TCP, routing | ⏳ Phase 3 |
| `test_graphics_stack.c` | 10 | GPU, compositor, display | ⏳ Phase 4 |
| `test_security_expanded.c` | 20 | Cap/NS/MAC/Sandbox | ✓ Yes |
| `test_power_management.c` | 10 | ACPI, suspend, thermal | ⏳ Phase 4 |
| `integration_suite.c` | 15 | Core subsystem flow | ✓ Yes |

## Test Categories

```
1. Boot Sequence (10)       ✓ Ready
2. Application Lifecycle (15) ✓ Ready
3. File System (15)         ⏳ Phase 3
4. Network Stack (15)       ⏳ Phase 3
5. Graphics Stack (10)      ⏳ Phase 4
6. Security (20)            ✓ Ready
7. Power Management (10)    ⏳ Phase 4
8. Core Subsystems (15)     ✓ Ready
```

## From Kernel Code

```c
// Run full expanded suite (110 tests)
run_expanded_integration_suite();

// Run specific category
run_integration_category(1);  // Boot tests
run_integration_category(6);  // Security tests

// Quick smoke test
run_integration_smoke_test();
```

## Test Output Cheat Sheet

```
[TEST] Test Name...          ← Test started
  ✓ Assertion message        ← Passed assertion
  ASSERTION FAILED: ...      ← Failed assertion
[PASS] Test Name             ← Test passed
[FAIL] Test Name             ← Test failed
[SKIP] Test Name: reason     ← Test skipped

==================================================================
  TEST SUMMARY
==================================================================
  Passed:  95
  Failed:  0
  Skipped: 15
==================================================================
```

## Common Tasks

### Add New Integration Test

```c
void test_my_integration(void) {
    TEST_START("My Integration Point");

    // Setup
    resource_t* res = setup_resource();
    if (!res) {
        TEST_SKIP("My Integration Point", "Resource unavailable");
        return;
    }

    // Test
    int result = test_integration_point(res);
    TEST_ASSERT(result == SUCCESS, "Integration successful");

    // Cleanup
    cleanup_resource(res);

    TEST_END("My Integration Point");
}
```

### Debug Failed Test

1. Check test output for failed assertion
2. Verify subsystem initialization order
3. Check for resource exhaustion
4. Enable verbose logging
5. Run test in isolation: `run_integration_category(N)`

### Skip Test Temporarily

```c
TEST_SKIP("Test Name", "Reason for skipping");
```

## Test Quality Checklist

- [ ] Test is atomic (tests ONE integration point)
- [ ] Test has clear failure message
- [ ] Test runs in < 100ms
- [ ] Test is deterministic (no flaky behavior)
- [ ] Test cleans up resources
- [ ] Test is independent (no ordering dependency)

## Performance Targets

| Metric | Target | Importance |
|--------|--------|------------|
| Individual test | < 100ms | Medium |
| Category suite | < 60s | Medium |
| Full suite | < 5min | High |
| Smoke test | < 30s | High |

## Coverage at a Glance

```
Total:     110 tests
Ready Now: 60 tests (55%)
Phase 3:   30 tests (27%)
Phase 4:   20 tests (18%)
```

## Key Integration Points

```
Boot:     UEFI → Bootloader → Kernel → Drivers → Services
Memory:   PMM → VMM → Heap → Process allocator
Security: Capabilities ↔ Namespaces ↔ MAC ↔ Sandbox ↔ Audit
FS:       VFS ↔ Block layer ↔ Storage driver
Network:  Socket ↔ TCP ↔ IP ↔ Ethernet ↔ NIC driver
Graphics: GPU driver ↔ Compositor ↔ Application
Power:    ACPI ↔ Power manager ↔ Device drivers
```

## Quick Commands

```bash
# Statistics
make -f Makefile.expanded stats

# Help
make -f Makefile.expanded help

# Clean
make -f Makefile.expanded clean

# Run boot tests only
make -f Makefile.expanded category-1

# Run security tests only
make -f Makefile.expanded category-6
```

## Files to Edit

When adding tests:
1. Add test function to appropriate `test_*.c` file
2. Declare `extern void test_*()` in `integration_suite_expanded.c`
3. Call test function in appropriate runner
4. Update `Makefile.expanded` if new file
5. Update `EXPANDED_TEST_SUITE.md` documentation

## Documentation

- **Full Guide**: `EXPANDED_TEST_SUITE.md` (comprehensive)
- **This File**: `QUICK_REFERENCE.md` (quick lookup)
- **Report**: `EXPANSION_REPORT.md` (metrics and status)

---

**Need Help?**
- Read `EXPANDED_TEST_SUITE.md` for detailed guide
- Check `EXPANSION_REPORT.md` for current status
- Review existing test files for examples
