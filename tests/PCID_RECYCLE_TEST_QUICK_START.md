# PCID Recycle IPI Test - Quick Start

**One-command test for BUG-013 fix verification**

## TL;DR

```bash
# From WSL Arch in kernel root
bash scripts/test_pcid_recycle.sh --cpus 4
```

Expected: `✓ ALL TESTS PASSED` in ~30 seconds

---

## What It Tests

1. **IPI Broadcast**: All CPUs receive TLB flush IPI when PCID pool exhausts (next_pcid >= 4096)
2. **TLB Coherence**: No stale TLB entries on any CPU after flush
3. **System Stability**: Address space allocation works after PCID recycling

## Quick Validation (Manual)

### Step 1: Build with test enabled

```bash
cd /mnt/c/Users/wilde/Desktop/Kernel

# Compile test
gcc -c tests/test_pcid_recycle_ipi.c -o build/test_pcid_recycle_ipi.o \
    -I kernel/include -std=gnu11 -O2 -ffreestanding -nostdlib \
    -mno-red-zone -fno-stack-protector -DTEST_PCID_RECYCLE -DSMP_FOUNDATION

# Link into kernel (add to KERNEL_OBJS in Makefile)
# Then rebuild kernel
CFLAGS="-DTEST_PCID_RECYCLE -DSMP_FOUNDATION" bash scripts/quick_build.sh
```

### Step 2: Add test hook to IPI handler

Edit `kernel/arch/x86_64/ipi.c`:

```c
// At top
extern void test_pcid_ipi_hook(void);

// In ipi_handle_tlb_flush()
void ipi_handle_tlb_flush(void) {
    uint32_t cpu = cpu_id();
    ipi_stats[cpu].tlb_flush_received++;

    #ifdef TEST_PCID_RECYCLE
    test_pcid_ipi_hook();  // ADD THIS LINE
    #endif

    tlb_handle_ipi_flush();
    // ... rest
}
```

### Step 3: Invoke test from kernel_main

Edit `kernel/main.c`:

```c
extern void test_pcid_recycle_ipi_suite(void);

void kernel_main(void) {
    // ... after smp_init() ...

    #ifdef TEST_PCID_RECYCLE
    if (smp_num_cpus > 1) {
        test_pcid_recycle_ipi_suite();
    }
    #endif

    // ... rest ...
}
```

### Step 4: Boot and observe

```bash
grub-mkrescue -o build/automationos.iso iso/

qemu-system-x86_64 -cdrom build/automationos.iso \
    -m 512M -smp 4 -serial stdio -enable-kvm -cpu host
```

Look for:

```
✓ ALL TESTS PASSED
  BUG-013 fix verified: PCID recycling correctly broadcasts IPI
```

---

## Expected Output (Success)

```
╔════════════════════════════════════════════════════════════╗
║  PCID Recycling IPI Broadcast Validation Suite            ║
╠════════════════════════════════════════════════════════════╣
║  BUG-013 Fix Verification                                  ║
╚════════════════════════════════════════════════════════════╝

[TEST] PCID Recycle IPI Broadcast
  CPUs detected: 4
  Creating 4100 address spaces to trigger PCID recycling...
  PCID threshold crossed at iteration 4096
  Checking IPI delivery:
    CPU 1: received 1 TLB flush IPIs
    CPU 2: received 1 TLB flush IPIs
    CPU 3: received 1 TLB flush IPIs
  ✓ IPI broadcast detected on all 3 remote CPUs
  PASS: PCID recycling triggered IPI broadcast

[TEST] No Stale TLB Entries After PCID Recycle
  ✓ All 4 CPUs verified TLB flush
  PASS: No stale TLB entries detected

[TEST] Smoke Test After PCID Recycle
  ✓ All address spaces created successfully
  ✓ PCIDs properly recycled
  PASS: System stable after PCID recycle

========================================
PCID Recycle IPI Test Summary
========================================
  Total:  3 tests
  Passed: 3 tests
  Failed: 0 tests

✓ ALL TESTS PASSED
```

---

## Troubleshooting

| Issue | Fix |
|-------|-----|
| "Test requires at least 2 CPUs" | Use `--cpus 4` in QEMU |
| "Only 0/3 CPUs received IPI" | Add `-DSMP_FOUNDATION` to build |
| Test hook not called | Verify `#ifdef TEST_PCID_RECYCLE` in ipi.c |
| Kernel panic during test | Increase QEMU memory: `-m 1G` |

---

## CI Integration

Add to smoke test suite:

```bash
# In scripts/smoke.sh
bash scripts/test_pcid_recycle.sh --cpus 4 --timeout 60

if [[ $? -eq 0 ]]; then
    echo "[SMOKE] PCID recycle IPI: PASS"
else
    echo "[SMOKE] PCID recycle IPI: FAIL"
    exit 1
fi
```

---

## Files Modified

- **New**: `tests/test_pcid_recycle_ipi.c` (test implementation)
- **New**: `scripts/test_pcid_recycle.sh` (test runner)
- **Modified**: `kernel/arch/x86_64/ipi.c` (add test hook)
- **Modified**: `kernel/main.c` (invoke test suite)

---

## Verification Checklist

- [ ] Test compiles without errors
- [ ] All 3 tests pass
- [ ] IPI counts increment on all remote CPUs
- [ ] No kernel panics or allocation failures
- [ ] PCIDs recycled correctly (< 4096)
- [ ] Test completes in < 30 seconds

---

## Related Documentation

- Full test specification: `tests/PCID_RECYCLE_IPI_TEST.md`
- BUG-013 report: `docs/reports/BUG_DATABASE.md`
- SMP architecture: `docs/SMP_ARCHITECTURE.md`
- PCID implementation: `kernel/arch/x86_64/paging.c:787-801`
