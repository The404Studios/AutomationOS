# Driver Integration Test Guide
**Quick Reference for Running Driver Integration Tests**

---

## Quick Start

### Build and Run All Integration Tests
```bash
cd tests/drivers
make test-integration
```

### Run Specific Test Case
```bash
./driver_test_suite --test driver_integration::pci_init
./driver_test_suite --test driver_integration::load_order
```

---

## Available Test Cases

### Task 1: Driver Inventory
- `driver_inventory` - List all drivers and dependencies
- `dependency_resolution` - Verify dependency resolution

### Task 2: PCI Driver
- `pci_init` - Initialize PCI bus
- `pci_enumeration` - Enumerate PCI devices
- `pci_device_drivers` - Load device-specific drivers

### Task 3: ACPI Driver
- `acpi_init` - Initialize ACPI subsystem
- `acpi_tables` - Parse ACPI tables
- `acpi_power` - ACPI power management

### Task 4: Display Driver
- `display_init` - Initialize display driver

### Task 5: Input Drivers
- `keyboard_driver` - PS/2 keyboard
- `mouse_driver` - PS/2 mouse

### Task 6: Storage Drivers
- `ahci_driver` - AHCI/SATA driver
- `nvme_driver` - NVMe driver

### Task 7: USB Driver
- `usb_controller` - xHCI controller init
- `usb_enumeration` - USB device enumeration

### Task 9: Load Order
- `load_order` - Verify correct driver load order

### Task 10: Error Handling
- `missing_device` - Handle missing devices
- `init_failure` - Handle init failures
- `graceful_degradation` - System continues with partial drivers

---

## Test Results Interpretation

### Success (✅)
```
[PASS] driver_integration::pci_init
  [INFO] PCI bus registered successfully
```

### Failure (❌)
```
[FAIL] driver_integration::acpi_init
  [ERROR] test_driver_integration.c:123: Assertion failed
```

### Skipped (⚠️)
```
[SKIP] driver_integration::network_driver
  [INFO] Network driver not yet implemented
```

---

## Expected Load Order

1. **PCI Bus** (Priority 1)
2. **ACPI** (Priority 2) - depends on PCI
3. **IRQ Controller** (Priority 3) - depends on ACPI
4. **Timer** (Priority 4) - depends on IRQ
5. **Storage** (Priority 5) - depends on PCI, IRQ
6. **Display** (Priority 6) - depends on PCI
7. **Input** (Priority 7) - depends on IRQ
8. **USB** (Priority 8) - depends on PCI, IRQ
9. **Network** (Priority 9) - depends on PCI, IRQ

---

## Common Issues

### Issue: PCI bus not initialized
**Symptom:** Other tests fail with "PCI not initialized"  
**Fix:** Ensure `pci_init` test passes first

### Issue: Dependencies not met
**Symptom:** Driver load fails with dependency error  
**Fix:** Check that prerequisite drivers loaded successfully

### Issue: Device not detected
**Symptom:** "No devices found" in enumeration test  
**Fix:** Verify PCI enumeration is working, check mock devices

---

## Test Output Files

After running tests, check:
- `DRIVER_INTEGRATION_REPORT.md` - Full test report
- Terminal output - Real-time test progress
- Load order records - See driver initialization sequence

---

## Debugging Tips

### Enable Verbose Output
```bash
./driver_test_suite --test driver_integration --verbose
```

### Run Single Test
```bash
./driver_test_suite --test driver_integration::pci_init
```

### Check Memory Leaks
```bash
make valgrind
```

### View Load Order
The test automatically prints load order at the end:
```
=== Driver Load Order Report ===
Order | Driver          | Status  | Time
------|-----------------|---------|----------
  1   | pci             | SUCCESS | 0 us
  2   | acpi            | SUCCESS | 1250 us
  ...
```

---

## Integration with CI/CD

### Run in CI Pipeline
```yaml
test:
  script:
    - cd tests/drivers
    - make test-integration
  artifacts:
    reports:
      junit: test_results.xml
```

### Expected CI Output
```
✓ All tests passed (19/19)
✓ No memory leaks detected
✓ Driver load order correct
```

---

## Troubleshooting

### All Tests Failing
1. Check if test framework initialized
2. Verify kernel headers are accessible
3. Ensure test environment is set up

### Specific Driver Failing
1. Check driver dependencies
2. Review driver implementation
3. Check for missing PCI/ACPI initialization

### Load Order Incorrect
1. Review dependency graph
2. Check driver priorities
3. Ensure no circular dependencies

---

## Reporting Bugs

When reporting issues, include:
1. Full test output
2. Load order report
3. System information
4. Steps to reproduce

---

## Next Steps After Testing

1. ✅ All tests pass → Ready for boot integration
2. ⚠️  Some tests fail → Fix failing drivers
3. ❌ Critical tests fail → Review architecture

---

**For detailed results, see:** `DRIVER_INTEGRATION_REPORT.md`
