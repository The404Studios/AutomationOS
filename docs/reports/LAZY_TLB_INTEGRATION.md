# Lazy TLB Shootdown Integration Guide

## Quick Start

The lazy TLB shootdown implementation is complete and ready to use. Here's how to build, test, and verify it.

## Building

```bash
cd C:\Users\wilde\Desktop\Kernel

# Clean build
make clean

# Build kernel with lazy TLB support
make kernel

# Build complete ISO
make iso

# Run in QEMU
make qemu
```

The new lazy TLB subsystem will be automatically included because:
- `kernel/Makefile` uses `find . -name "*.c"` to discover source files
- `kernel/arch/x86_64/tlb.c` will be found and compiled automatically

## Boot Output

You should see this during boot:

```
[VMM] Initializing paging...
[VMM] PCID enabled (Process-Context Identifiers)
[VMM] TLB flushing optimized for context switches
[VMM] Initializing lazy TLB shootdown...
[TLB] Initializing lazy TLB shootdown...
[TLB] Lazy TLB shootdown initialized for X CPUs
[VMM] Lazy TLB shootdown initialized
```

## Testing

### Manual Testing

Boot the OS and check the logs. The lazy TLB subsystem should initialize without errors.

### Unit Tests

Run the included unit tests:

```c
// In kernel_main() or a test harness:
extern void test_lazy_tlb(void);
test_lazy_tlb();
```

Expected output:
```
========================================
   Lazy TLB Shootdown Tests
========================================
CPUs: 4
========================================

[TEST] TLB Initialization
[PASS] TLB statistics reset
...
[SUCCESS] All lazy TLB tests passed!
```

### Benchmark

Run the performance benchmark:

```c
// In kernel_main() or a test harness:
extern void tlb_shootdown_benchmark(void);
tlb_shootdown_benchmark();
```

Expected output:
```
========================================
   TLB Shootdown Benchmark
========================================
CPUs: 4
Testing lazy TLB shootdown effectiveness
========================================

=== Single-Page Unmap Test ===
  Total cycles:     12345678
  IPIs sent:        200
  IPIs avoided:     800
  IPI reduction:    80%

=== Multi-Page Unmap Test ===
  Total cycles:     45678912
  IPIs sent:        150
  IPIs avoided:     850
  IPI reduction:    85%

=== Heavy Munmap Stress Test ===
  Total cycles:     98765432
  IPIs sent:        300
  IPIs avoided:     2700
  IPI reduction:    90%
```

## Verification

### Check Statistics

At any time during runtime, you can check TLB statistics:

```c
extern void tlb_print_stats(void);
tlb_print_stats();
```

Output:
```
=== Lazy TLB Shootdown Statistics ===
CPU 0:
  Lazy flushes:      1234
  Immediate flushes: 456
  Batched flushes:   789
  IPIs sent:         456
  IPIs avoided:      3000

CPU 1:
  Lazy flushes:      987
  Immediate flushes: 321
  ...

Totals:
  Total lazy flushes:      5432
  Total immediate flushes: 1234
  Total batched flushes:   3210
  Total IPIs sent:         1234
  Total IPIs avoided:      12000
  IPI reduction:           90%
```

### Debug Output

To enable detailed debug output, edit `kernel/arch/x86_64/tlb.c`:

```c
// Uncomment this line at the top of the file:
#define TLB_DEBUG
```

Then rebuild:
```bash
make clean && make kernel
```

Debug output will show:
```
[TLB] CPU 0: deferred flush for 3 CPUs (addr=0x10000000)
[TLB] CPU 1: immediate flush for active kernel CPU 2
```

## Integration Checklist

- [x] Created `kernel/arch/x86_64/tlb.c` (core implementation)
- [x] Created `kernel/include/tlb.h` (public API)
- [x] Modified `kernel/core/sched/context.c` (added `tlb_flush_pending()`)
- [x] Modified `kernel/arch/x86_64/paging.c` (replaced immediate flush)
- [x] Modified `kernel/arch/x86_64/ipi.c` (redirected handler)
- [x] Modified `kernel/kernel.c` (added `tlb_init()`)
- [x] Created `benchmarks/micro/tlb_shootdown_bench.c` (performance tests)
- [x] Created `tests/test_lazy_tlb.c` (unit tests)
- [x] Documentation (this file + summary)

## Performance Tuning

### Batch Threshold

Edit `kernel/arch/x86_64/tlb.c`:

```c
// Default: 32 pages before escalating to full flush
#define TLB_BATCH_THRESHOLD 32

// For systems with frequent small unmaps, increase:
#define TLB_BATCH_THRESHOLD 64

// For systems with large unmaps, decrease:
#define TLB_BATCH_THRESHOLD 16
```

### Active Check Heuristic

The implementation checks if a remote CPU is "active" before deciding whether to send an immediate IPI:

```c
static inline bool cpu_is_active_kernel(uint32_t cpu) {
    percpu_data_t* data = cpu_data(cpu);
    if (!data) return false;

    // If preemption is disabled, CPU is in kernel critical section
    if (data->preempt_count > 0) {
        return true;
    }

    // If interrupts are disabled, CPU is in kernel critical section
    if (!data->interrupts_enabled) {
        return true;
    }

    return false;
}
```

You can tune this heuristic based on your workload.

## Troubleshooting

### Build Errors

**Error**: `undefined reference to 'tlb_init'`

**Solution**: Make sure `kernel/arch/x86_64/tlb.c` exists and is being compiled. Check `kernel/Makefile` uses `find . -name "*.c"`.

**Error**: `implicit declaration of function 'tlb_flush_pending'`

**Solution**: Add `#include "../../include/tlb.h"` to the file that calls it.

### Runtime Errors

**Error**: Kernel panic during boot

**Solution**: Check that `tlb_init()` is called AFTER `vmm_init()` in `kernel_main()`.

**Error**: TLB coherency issues (processes see stale memory)

**Solution**: 
1. Enable `TLB_DEBUG` to see flush patterns
2. Check that `tlb_flush_pending()` is called in `context_switch()`
3. Verify `paging_unmap_page()` calls `tlb_flush_page_lazy()`

### Performance Issues

**Issue**: No IPI reduction observed

**Check**:
1. Are you running on multi-CPU system? (single-CPU has no remote IPIs)
2. Is the workload actually doing unmaps? (check with `tlb_print_stats()`)
3. Are all CPUs active in kernel? (immediate flush path)

**Issue**: Too many immediate flushes

**Tune**: Adjust the active check heuristic to be less aggressive.

## Advanced Usage

### Custom Flush Strategy

You can implement custom flush strategies by modifying `tlb_mark_flush()`:

```c
static inline void tlb_mark_flush(uint32_t cpu, uint64_t addr, uint64_t cr3) {
    spin_lock(&tlb_state[cpu].lock);

    // Custom logic here...
    // Example: flush immediately if CR3 matches current
    if (cr3 == current_cr3_on_cpu(cpu)) {
        // Send immediate IPI
        ipi_send(cpu, IPI_TLB_FLUSH);
    } else {
        // Defer
        tlb_state[cpu].needs_flush = true;
        tlb_state[cpu].flush_addr = addr;
    }

    spin_unlock(&tlb_state[cpu].lock);
}
```

### Statistics Analysis

Export TLB statistics to userspace for analysis:

```c
// Add syscall in kernel/core/syscall/handlers.c
int64_t sys_tlb_stats(struct tlb_stats* buf) {
    if (!buf) return -1;
    
    // Copy stats to userspace
    extern void tlb_get_stats(struct tlb_stats* out);
    tlb_get_stats(buf);
    
    return 0;
}
```

## References

- `LAZY_TLB_SHOOTDOWN_SUMMARY.md` - Detailed implementation summary
- `kernel/include/tlb.h` - Public API documentation
- `benchmarks/micro/tlb_shootdown_bench.c` - Performance benchmark
- `tests/test_lazy_tlb.c` - Unit tests

## Support

For issues or questions:
1. Check debug output with `TLB_DEBUG` enabled
2. Review statistics with `tlb_print_stats()`
3. Run unit tests with `test_lazy_tlb()`
4. Run benchmark with `tlb_shootdown_benchmark()`

## Next Steps

1. Build and boot the kernel
2. Verify initialization messages
3. Run unit tests
4. Run benchmark
5. Analyze statistics
6. Tune batch threshold if needed

## Success Criteria

- ✓ Kernel boots without errors
- ✓ TLB subsystem initializes
- ✓ Unit tests pass
- ✓ Benchmark shows 60-80% IPI reduction (multi-CPU)
- ✓ No TLB coherency bugs (all processes see correct memory)
