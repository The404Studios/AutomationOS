# AutomationOS Performance Quick Reference

Quick reference card for kernel developers.

---

## Using Performance Counters

```c
#include "perf.h"

// Basic timing
PERF_TIMER_START();
// ... code to measure ...
PERF_TIMER_END("operation_name");

// Accurate timing (with fencing)
PERF_TIMER_START_ACCURATE();
// ... code ...
PERF_TIMER_END_ACCURATE("operation_name");

// Manual timing
uint64_t start = rdtsc();
// ... code ...
uint64_t cycles = rdtsc() - start;
kprintf("Took %llu cycles (%.2f us)\n", cycles, cycles_to_us(cycles));
```

---

## Performance Budget (Estimated)

### Fast Path Operations (< 100 cycles)
- ✓ Stack push/pop: ~1-2 cycles
- ✓ MOV instruction: ~1 cycle
- ✓ Function call (near): ~5-10 cycles
- ✓ L1 cache hit: ~4-5 cycles
- ✓ L2 cache hit: ~12-15 cycles

### Medium Operations (100-1000 cycles)
- ✓ Syscall entry/exit: ~150-250 cycles
- ✓ Context switch (best case): ~300-570 cycles
- ✓ L3 cache hit: ~40-50 cycles
- ✓ PMM page allocation: ~100-500 cycles
- ✓ Heap allocation: ~50-200 cycles

### Expensive Operations (> 1000 cycles)
- ⚠ TLB miss (hardware walk): ~100-200 cycles
- ⚠ Page fault: ~2,000-10,000 cycles
- ⚠ Process fork: ~10,000-50,000 cycles
- ⚠ Memory miss to RAM: ~200-300 cycles
- ⚠ Serial character output (busy-wait): ~780,000 cycles!

---

## Hot Path Rules

### DO:
- ✓ Inline small functions (<10 instructions)
- ✓ Use likely/unlikely hints for branches
- ✓ Keep data structures cache-line aligned
- ✓ Batch operations when possible
- ✓ Use per-CPU data to avoid locks
- ✓ Prefetch data when access pattern is known

### DON'T:
- ✗ Call kprintf() in hot paths
- ✗ Allocate memory in interrupt context
- ✗ Hold locks across I/O operations
- ✗ Use floating point in kernel
- ✗ Busy-wait for hardware
- ✗ Disable interrupts for long periods

---

## Common Bottlenecks

### Context Switch
**Cost:** ~300-570 cycles  
**Hotspot:** TLB flush (200-400 cycles)  
**Fix:** Implement PCID

### Syscall
**Cost:** ~150-250 cycles  
**Hotspot:** Argument shuffling  
**Fix:** Use SYSRET instead of JMP

### Memory Allocation
**Cost:** ~100-500 cycles  
**Hotspot:** Free list scanning  
**Fix:** Add per-CPU cache

### Serial Output
**Cost:** ~780,000 cycles per char!  
**Hotspot:** Busy-wait loop  
**Fix:** Interrupt-driven transmission

---

## Optimization Checklist

### Before Optimizing
- [ ] Profile first - measure actual performance
- [ ] Identify hot path (where is 80% of time spent?)
- [ ] Set performance goal (target cycles/throughput)
- [ ] Create benchmark to validate improvement

### Optimization Techniques
- [ ] Remove unnecessary work
- [ ] Reduce memory allocations
- [ ] Improve cache locality
- [ ] Eliminate locks on fast path
- [ ] Use lock-free algorithms
- [ ] Batch operations
- [ ] Defer work to bottom-half

### After Optimizing
- [ ] Re-run benchmark
- [ ] Verify improvement
- [ ] Check for regressions
- [ ] Document changes
- [ ] Add regression test

---

## Profiling Commands

```bash
# Build with performance counters
make PERF=1

# Run specific benchmark
make qemu-bench-ctxsw      # Context switch
make qemu-bench-syscall    # Syscall latency
make qemu-bench-memory     # Memory allocation

# Run all benchmarks
make bench-all

# Profile boot time
make qemu-bench-boot

# Generate report
python3 tests/perf/generate_report.py
```

---

## Cycle Counts Reference

### x86_64 Instructions
```
MOV reg, reg          1 cycle
ADD/SUB reg, reg      1 cycle
IMUL reg, reg         3 cycles
IDIV reg              25-30 cycles
PUSH/POP              1-2 cycles
CALL (near)           5-10 cycles
RET                   5-10 cycles
SYSCALL               60-100 cycles
SYSRET                60-100 cycles
IRETQ                 50-100 cycles
```

### Memory Access
```
L1 cache hit          4-5 cycles
L2 cache hit          12-15 cycles
L3 cache hit          40-50 cycles
RAM access            200-300 cycles
TLB miss              100-200 cycles
Page fault            2,000-10,000 cycles
```

### Kernel Operations
```
kprintf (1 char)      ~780,000 cycles (busy-wait!)
Context switch        ~300-570 cycles
Syscall (null)        ~150-250 cycles
PMM alloc page        ~100-500 cycles
Heap alloc (small)    ~50-200 cycles
Interrupt entry/exit  ~185-370 cycles
```

---

## Cache-Friendly Coding

### Struct Packing
```c
// BAD: 24 bytes, poor cache usage
struct bad {
    char a;      // 1 byte
    int b;       // 4 bytes (3 bytes padding)
    char c;      // 1 byte
    void* d;     // 8 bytes (7 bytes padding)
};

// GOOD: 16 bytes, cache-line friendly
struct good {
    void* d;     // 8 bytes
    int b;       // 4 bytes
    char a;      // 1 byte
    char c;      // 1 byte
    // 2 bytes padding
} __attribute__((aligned(64)));  // Align to cache line
```

### Hot/Cold Splitting
```c
struct process {
    // HOT: Frequently accessed in scheduler
    uint32_t pid;
    uint32_t state;
    uint64_t time_slice;
    struct process* next;
    
    // COLD: Rarely accessed
    char name[256];
    uint64_t create_time;
    // ...
};
```

---

## Lock-Free Patterns

### Per-CPU Data (No Lock Needed)
```c
static __thread int my_cpu_data;  // Thread-local

void fast_path(void) {
    my_cpu_data++;  // No lock!
}
```

### Atomic Operations
```c
volatile uint64_t counter = 0;

// Atomic increment
__atomic_fetch_add(&counter, 1, __ATOMIC_SEQ_CST);

// Compare-and-swap
uint64_t old = 0, new = 1;
__atomic_compare_exchange(&counter, &old, &new, ...);
```

---

## Performance Anti-Patterns

### 1. Debug Logging in Hot Path
```c
// BAD
void schedule(void) {
    kprintf("[SCHED] Scheduling...\n");  // ~780K cycles!
    // ...
}

// GOOD
#ifdef DEBUG
    kprintf("[SCHED] Scheduling...\n");
#endif
```

### 2. Memory Allocation in Interrupt
```c
// BAD
void irq_handler(void) {
    void* buf = kmalloc(1024);  // Can block!
    // ...
}

// GOOD
static char irq_buffer[1024];  // Pre-allocated
void irq_handler(void) {
    // Use irq_buffer
}
```

### 3. Holding Lock Across I/O
```c
// BAD
spin_lock(&lock);
serial_write(...);  // Blocks!
spin_unlock(&lock);

// GOOD
serial_write(...);
spin_lock(&lock);
// Critical section
spin_unlock(&lock);
```

---

## Benchmarking Best Practices

### 1. Warm Up Cache
```c
// Warmup
for (int i = 0; i < 1000; i++) {
    function_under_test();
}

// Actual measurement
uint64_t start = rdtsc_fence();
for (int i = 0; i < 10000; i++) {
    function_under_test();
}
uint64_t end = rdtscp();
```

### 2. Prevent Compiler Optimization
```c
// Compiler might optimize away
int result = expensive_function();

// Force execution
volatile int result = expensive_function();
```

### 3. Statistical Analysis
```c
perf_stat_t stats;
perf_stat_init(&stats, "operation");

for (int i = 0; i < 1000; i++) {
    uint64_t start = rdtsc_fence();
    operation();
    uint64_t end = rdtscp();
    perf_stat_record(&stats, end - start);
}

perf_stat_report(&stats);  // min/avg/max
```

---

## When to Optimize

### Premature Optimization is Evil
- DON'T optimize before profiling
- DON'T optimize code that runs once
- DON'T sacrifice readability for 5% speedup

### Worthwhile Optimization
- DO optimize hot paths (>10% of runtime)
- DO optimize when measured impact is significant
- DO optimize when it simplifies code

### Use This Priority Order
1. **Correctness** - Make it work
2. **Readability** - Make it clear
3. **Performance** - Make it fast (if needed)

---

## Performance Goals (Phase 1)

| Metric | Current | Target | Status |
|--------|---------|--------|--------|
| Boot time | Unknown | <500ms | Need measurement |
| Context switch | ~300-570 | <250 | Implement PCID |
| Syscall | ~150-250 | <150 | Implement SYSRET |
| Page alloc | ~100-500 | <50 | Add per-CPU cache |
| Interrupt latency | ~185-370 | <200 | Split top/bottom half |

---

## Resources

- **Full Analysis:** `docs/PHASE1_PERFORMANCE_PROFILE.md`
- **Summary:** `docs/PERFORMANCE_SUMMARY.md`
- **API Reference:** `kernel/include/perf.h`
- **Benchmarks:** `tests/bench/`

---

**Remember:** Measure first, optimize second!

*"Premature optimization is the root of all evil." - Donald Knuth*
