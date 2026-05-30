# SMP Testing Quick Start Guide

**Get your SMP implementation validated in 5 minutes**

## Prerequisites

- AutomationOS kernel source code
- GCC toolchain
- QEMU (qemu-system-x86_64)
- 10 minutes of time

## Step 1: Verify Files (30 seconds)

```bash
cd /path/to/AutomationOS
cd tests/

# Check test files exist
ls -lh test_smp.c              # Main test suite
ls -lh run_smp_tests.sh        # Test runner
ls -lh Makefile.smp            # Build file
ls -lh README_SMP_VALIDATION.md # Full docs
```

## Step 2: Quick QEMU Test (2 minutes)

**Run a single test with 4 CPUs:**

```bash
# Make sure you're in the tests/ directory
cd tests/

# Run test script
./run_smp_tests.sh single 4 512

# Check result
tail -20 smp_test_4cpu_512mb.log
```

**Expected Output:**
```
================================================================================
                           TEST SUMMARY
================================================================================

Tests run: 9
Passed: 9
Failed: 0

Result: ALL TESTS PASSED

================================================================================
```

## Step 3: Full Test Suite (5 minutes)

**Test multiple CPU configurations:**

```bash
# Run all configurations (2, 4, 8, 16 CPUs)
./run_smp_tests.sh all

# View results summary
./run_smp_tests.sh results
```

**Expected Output:**
```
================================
  Test Summary
================================
  Total configurations: 4
  Passed: 4
  Failed: 0

SUCCESS: All tests passed!
```

## Step 4: Review Results (2 minutes)

**Check detailed logs:**

```bash
# List all test logs
ls -lh smp_test_*.log

# View specific test
less smp_test_8cpu_1024mb.log

# Search for failures
grep -i "FAIL\|ERROR\|WARNING" smp_test_*.log

# Extract performance data
grep "Speedup:\|Efficiency:" smp_test_*.log
```

## Common Results

### ✅ Success (All Tests Pass)

```
[PASS] CPU Detection (125 us)
[PASS] AP Startup (312 us)
[PASS] Per-CPU Data Isolation (89 us)
[PASS] IPI Delivery (234 us)
[PASS] IPI Latency (1245 us)
[PASS] TLB Shootdown (156 us)
[PASS] Cache Coherence (2341 us)
[PASS] Performance Scaling (31244 us)
[PASS] Stress Test (5002341 us)

Result: ALL TESTS PASSED
```

**Action**: ✅ SMP implementation working correctly!

### ⚠️ Warning (Some Tests Pass with Warnings)

```
[PASS] Performance Scaling (31244 us)
  WARNING: Efficiency below target for 8 CPUs (target: >90%)
    Speedup: 6.8x
    Parallel Efficiency: 85.2%

Result: ALL TESTS PASSED
```

**Action**: ⚠️ Functional but performance below ideal. Investigate contention.

### ❌ Failure (Test Fails)

```
[FAIL] AP Startup: Only 3/4 CPUs came online

Result: 1 TEST(S) FAILED
```

**Action**: ❌ Critical issue. See troubleshooting section.

## Troubleshooting

### No CPUs Detected

**Symptom**: `[FAIL] CPU Detection: No CPUs detected`

**Fix**:
```bash
# Check ACPI support
grep -i acpi kernel.log

# Verify MADT table
# In kernel, add: acpi_dump_tables();
```

### APs Won't Boot

**Symptom**: `[FAIL] AP Startup: Only 1/N CPUs came online`

**Fix**:
```bash
# Increase boot timeout
# Edit kernel/arch/x86_64/smp.c, line 276:
# Change: uint32_t timeout = 100000;
# To:     uint32_t timeout = 1000000;

# Verify trampoline location
# Should be at 0x8000
```

### Poor Performance

**Symptom**: `WARNING: Efficiency below target`

**Fix**:
```bash
# Check for lock contention
# Enable lockdep in kernel config

# Check CPU frequency
# May be throttled in QEMU
# Use: -cpu host,+x2apic

# Check memory
# Increase with: -m 2G
```

### IPI Timeout

**Symptom**: `[FAIL] IPI Delivery: CPU N did not receive IPI`

**Fix**:
```bash
# Check LAPIC enabled
# Should see: [LAPIC] CPU APIC ID: N, Version: 0xNN

# Check for LAPIC errors
# Add to test: lapic_get_error()

# Try x2APIC mode
# Boot with -cpu host,+x2apic
```

## Real Hardware Testing

### Step 1: Build Bootable Image

```bash
# Build kernel with SMP tests
cd ..
make clean
make EXTRA_CFLAGS="-DRUN_SMP_TESTS"

# Create bootable ISO
make iso
```

### Step 2: Boot from USB

```bash
# Write to USB (CAREFUL - will erase USB!)
sudo dd if=kernel.iso of=/dev/sdX bs=4M status=progress
sync

# Boot from USB
# Watch serial console or screen output
```

### Step 3: Capture Results

- Take photo of screen showing test results
- Or capture serial output to file
- Document hardware configuration

## Integration with Kernel

### Option 1: Boot Parameter

**Quick integration** (no code changes):

```bash
# Modify boot loader config to add:
kernel.bin test_smp

# Or in GRUB:
menuentry "AutomationOS (SMP Test)" {
    multiboot /boot/kernel.bin test_smp
}
```

### Option 2: Always Run

**For development builds**:

```c
// In kernel/init/main.c
#ifdef DEBUG
    smp_run_tests();
#endif
```

Build with: `make DEBUG=1`

## Performance Targets

Quick reference for expected results:

| CPUs | Speedup | Efficiency | IPI Latency | TLB Flush |
|------|---------|------------|-------------|-----------|
| 2    | >1.9x   | >95%       | <5μs        | <6μs      |
| 4    | >3.7x   | >92%       | <6μs        | <10μs     |
| 8    | >7.2x   | >90%       | <8μs        | <15μs     |
| 16   | >13.6x  | >85%       | <10μs       | <20μs     |

**If your results are within 10% of these targets, you're good!**

## Next Steps

### If Tests Pass ✅

1. **Test on real hardware** with actual CPU count
2. **Run extended stress test** (24 hours)
3. **Benchmark real workloads** (not just synthetic)
4. **Document configuration** (CPU model, memory, etc.)
5. **Celebrate** 🎉 - Your SMP works!

### If Tests Fail ❌

1. **Read full docs**: `README_SMP_VALIDATION.md`
2. **Follow checklist**: `SMP_VALIDATION_CHECKLIST.md`
3. **Enable debug output**: Set `SMP_DEBUG=1`
4. **Check QEMU logs**: `-d int,cpu_reset -D debug.log`
5. **File bug report** with logs attached

### If You Want More

- **Full documentation**: See `README_SMP_VALIDATION.md`
- **Integration guide**: See `INTEGRATION_GUIDE.md`
- **Validation checklist**: See `SMP_VALIDATION_CHECKLIST.md`
- **Technical details**: See `SMP_VALIDATOR_REPORT.md`

## Getting Help

### Check Logs First

```bash
# Kernel boot log
cat kernel.log | grep -i "smp\|apic\|cpu"

# Test results
cat smp_test_*.log | grep -i "fail\|error"

# QEMU debug output
qemu-system-x86_64 ... -d int -D qemu_debug.log
```

### Common Error Messages

| Message | Meaning | Fix |
|---------|---------|-----|
| "MADT not found" | ACPI issue | Check ACPI init |
| "AP timeout" | CPU won't boot | Increase timeout |
| "IPI delivery timeout" | LAPIC issue | Check LAPIC init |
| "Cache coherence failure" | Memory issue | Check atomics |
| "Efficiency below target" | Performance | Check contention |

### Still Stuck?

1. Re-read the error message carefully
2. Check the troubleshooting section in README
3. Enable maximum debug output
4. Test with fewer CPUs (2 or 4)
5. Test in QEMU first, then real hardware
6. Ask for help with logs attached

## One-Liner Summary

```bash
# Test SMP in one command:
cd tests && ./run_smp_tests.sh && echo "✅ SMP WORKS!" || echo "❌ SMP BROKEN"
```

## Success Checklist

Before declaring victory:

- [ ] All 9 tests pass in QEMU
- [ ] Performance within 10% of targets
- [ ] Tested with 2, 4, and 8 CPUs
- [ ] No warnings or errors in logs
- [ ] Stress test completes without crashes
- [ ] Works on real hardware (if available)

## TL;DR

```bash
# The absolute minimum:
cd tests
./run_smp_tests.sh single 4 512
grep "ALL TESTS PASSED" smp_test_4cpu_512mb.log && echo "✅ GOOD TO GO"
```

**That's it! Your SMP implementation is validated.**

---

**Time to first result**: 2 minutes  
**Time to full validation**: 10 minutes  
**Confidence level**: 95%+ if all tests pass  

**Questions?** Read the full docs in `README_SMP_VALIDATION.md`

---

*Quick Start Guide v1.0 - 2026-05-26*
