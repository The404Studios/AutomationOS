# PCID Recycling IPI Broadcast Test

**BUG-013 Fix Verification**

Validates that PCID recycling correctly broadcasts TLB flush IPIs to all CPUs when the PCID pool exhausts (next_pcid >= 4096).

## Test Overview

The test suite validates three critical requirements:

1. **IPI Broadcast**: IPI is sent to all CPUs when `next_pcid >= 4096`
2. **TLB Coherence**: No stale TLB entries remain on remote CPUs after flush
3. **System Stability**: Smoke test passes after PCID recycling

## Architecture Context

### PCID Recycling Location

File: `kernel/arch/x86_64/paging.c`

```c
// Lines 787-801
if (next_pcid >= 4096) {
    kprintf("[PAGING] Warning: PCID exhausted, recycling from 1\n");
    kprintf("[PAGING] Flushing all TLB entries before PCID recycling\n");

    // Flush TLB on ALL CPUs by reloading CR3 without PCID preservation
    uint64_t cr3 = read_cr3() & ~CR3_NO_FLUSH;
    write_cr3(cr3);
    #ifdef SMP_FOUNDATION
    ipi_send_all_but_self(IPI_TLB_FLUSH);  // Broadcast to all CPUs
    #endif

    // TLB flushed on current CPU (and all remote CPUs via IPI if SMP)
    next_pcid = 1;
}
```

### IPI Handler Chain

1. **Sender** (CPU 0): `paging_create_address_space()` → `ipi_send_all_but_self(IPI_TLB_FLUSH)`
2. **LAPIC**: Broadcasts interrupt vector `0x41` to all APs
3. **Assembly Handler**: `kernel/arch/x86_64/ipi_handlers.asm` → `ipi_tlb_flush_handler`
4. **C Handler**: `kernel/arch/x86_64/ipi.c` → `ipi_handle_tlb_flush()`
5. **TLB Subsystem**: `kernel/arch/x86_64/tlb.c` → `tlb_handle_ipi_flush()`

## Test Implementation

### Test 1: IPI Broadcast Detection

**Method**:
- Maintain per-CPU IPI counters incremented in `ipi_handle_tlb_flush()`
- Create 4100 address spaces to trigger PCID exhaustion
- Verify all remote CPUs (CPU 1..N) received at least one IPI

**Pass Criteria**:
- All N-1 remote CPUs show non-zero IPI delta
- Broadcast detected message printed

**Failure Modes**:
- Some CPUs never receive IPI → broadcast incomplete
- No CPUs receive IPI → `#ifdef SMP_FOUNDATION` not defined

### Test 2: TLB Coherence Verification

**Method**:
- Create test address space with known mapping at `0x200000000`
- Use `ipi_call_function_many()` to execute verification on all CPUs
- Each CPU marks itself as "flushed" and records the test address

**Pass Criteria**:
- All N CPUs execute the verification function
- No page faults or stale translations detected

**Failure Modes**:
- Stale TLB entries → CPUs see old mappings
- IPI delivery failure → some CPUs don't execute function

### Test 3: Post-Recycle Smoke Test

**Method**:
- Create 10 new address spaces after PCID recycling
- Verify PCIDs are in range [1..4095] (recycled)
- Ensure all allocations succeed

**Pass Criteria**:
- 10/10 address spaces created
- All PCIDs < 4096
- No kernel panics or allocation failures

**Failure Modes**:
- PCID counter not reset → PCIDs still >= 4096
- Memory corruption → allocation failures

## Build Instructions

### Step 1: Compile Test Object

```bash
cd /mnt/c/Users/wilde/Desktop/Kernel
gcc -c tests/test_pcid_recycle_ipi.c -o build/test_pcid_recycle_ipi.o \
    -I kernel/include -std=gnu11 -O2 -ffreestanding -nostdlib \
    -mno-red-zone -fno-stack-protector -fno-pic -fno-pie
```

### Step 2: Link into Kernel

Add to `kernel/Makefile` or link command:

```makefile
KERNEL_OBJS += build/test_pcid_recycle_ipi.o
```

Or manually link:

```bash
ld -r build/test_pcid_recycle_ipi.o build/kernel_partial.o -o build/kernel.o
```

### Step 3: Add Test Hook to IPI Handler

Edit `kernel/arch/x86_64/ipi.c`:

```c
// Add extern declaration
extern void test_pcid_ipi_hook(void);

// In ipi_handle_tlb_flush():
void ipi_handle_tlb_flush(void) {
    uint32_t cpu = cpu_id();
    ipi_stats[cpu].tlb_flush_received++;

    // TEST HOOK: increment counter for PCID recycle test
    #ifdef TEST_PCID_RECYCLE
    test_pcid_ipi_hook();
    #endif

    // Use lazy TLB flush handler (flushes pending TLB entries)
    tlb_handle_ipi_flush();

    // ... rest of handler
}
```

### Step 4: Enable Test Build Flag

```bash
# In build command or Makefile
CFLAGS += -DTEST_PCID_RECYCLE
```

### Step 5: Invoke Test from Kernel Init

Edit `kernel/main.c`:

```c
extern void test_pcid_recycle_ipi_suite(void);

// After SMP initialization and before userspace launch
void kernel_main(void) {
    // ... existing initialization ...
    
    smp_init();  // Bring up APs
    
    #ifdef TEST_PCID_RECYCLE
    if (smp_num_cpus > 1) {
        test_pcid_recycle_ipi_suite();
    } else {
        kprintf("[TEST] Skipping PCID recycle test (single CPU)\n");
    }
    #endif
    
    // ... launch userspace ...
}
```

## Running the Test

### QEMU with 4 CPUs

```bash
cd /mnt/c/Users/wilde/Desktop/Kernel
bash scripts/quick_build.sh

# Boot with 4 cores
qemu-system-x86_64 -cdrom build/automationos.iso \
    -m 512M -smp 4 -serial stdio \
    -enable-kvm -cpu host
```

### Expected Output

```
╔════════════════════════════════════════════════════════════╗
║  PCID Recycling IPI Broadcast Validation Suite            ║
╟────────────────────────────────────────────────────────────╢
║  BUG-013 Fix Verification                                  ║
║  Validates TLB flush IPI broadcast when next_pcid >= 4096  ║
╚════════════════════════════════════════════════════════════╝

[TEST] PCID Recycle IPI Broadcast
========================================
  CPUs detected: 4
  Baseline IPI counts recorded
  Creating 4100 address spaces to trigger PCID recycling...
  PCID threshold crossed at iteration 4096
  Created 256 address spaces
  Checking IPI delivery:
    CPU 1: received 1 TLB flush IPIs
    CPU 2: received 1 TLB flush IPIs
    CPU 3: received 1 TLB flush IPIs
  ✓ IPI broadcast detected on all 3 remote CPUs
  PASS: PCID recycling triggered IPI broadcast

[TEST] No Stale TLB Entries After PCID Recycle
========================================
  Created test address space: CR3=0x...
  Allocated test page: phys=0x...
  Mapped test page at virt=0x200000000
  Broadcasting verification to all CPUs...
    CPU 0: verified (test_addr=0x200000000)
    CPU 1: verified (test_addr=0x200000000)
    CPU 2: verified (test_addr=0x200000000)
    CPU 3: verified (test_addr=0x200000000)
  ✓ All 4 CPUs verified TLB flush
  PASS: No stale TLB entries detected

[TEST] Smoke Test After PCID Recycle
========================================
  Creating 10 address spaces post-recycle...
    Created CR3=0x... (PCID=1)
    Created CR3=0x... (PCID=2)
    ...
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
  BUG-013 fix verified: PCID recycling correctly broadcasts IPI
```

## Troubleshooting

### Issue: "Only 0/3 CPUs received TLB flush IPI"

**Root Cause**: `SMP_FOUNDATION` not defined during build

**Fix**: Add `-DSMP_FOUNDATION` to CFLAGS:

```bash
CFLAGS="-DSMP_FOUNDATION" bash scripts/quick_build.sh
```

### Issue: "Test requires at least 2 CPUs (found 1)"

**Root Cause**: QEMU launched with 1 CPU or SMP not initialized

**Fix**: Boot with `-smp 4` and ensure `smp_init()` completes

### Issue: IPI counts don't increment

**Root Cause**: Test hook not called (missing `#ifdef TEST_PCID_RECYCLE`)

**Fix**: Verify `test_pcid_ipi_hook()` is called in `ipi_handle_tlb_flush()`

### Issue: Kernel panic during address space creation

**Root Cause**: PMM exhaustion or page table allocation failure

**Fix**: Increase QEMU memory (`-m 1G`) or reduce `TEST_PCID_CYCLES`

## Verification Checklist

- [ ] All 3 tests pass
- [ ] IPI broadcast detected on all N-1 remote CPUs
- [ ] No stale TLB entries on any CPU
- [ ] PCIDs recycled correctly (< 4096)
- [ ] No kernel panics or allocation failures
- [ ] Test completes in < 30 seconds
- [ ] Serial log shows PASS for all tests

## Integration with CI

Add to `scripts/smoke.sh`:

```bash
# After kernel boot and SMP init
if grep -q "✓ ALL TESTS PASSED" "$LOG"; then
    echo "[SMOKE] PCID recycle IPI test: PASS"
else
    echo "[SMOKE] PCID recycle IPI test: FAIL"
    exit 1
fi
```

## Related Files

- **Test**: `tests/test_pcid_recycle_ipi.c`
- **PCID Logic**: `kernel/arch/x86_64/paging.c:787-801`
- **IPI Handler**: `kernel/arch/x86_64/ipi.c:344-356`
- **TLB Subsystem**: `kernel/arch/x86_64/tlb.c`
- **Bug Report**: `docs/reports/BUG_DATABASE.md` (BUG-013)

## References

- Intel SDM Vol. 3A, Section 4.10.1: "Process-Context Identifiers (PCIDs)"
- Intel SDM Vol. 3A, Section 11.8: "Handling Interrupts"
- `docs/SMP_ARCHITECTURE.md`: IPI infrastructure
- `kernel/include/lapic.h`: IPI vector definitions
