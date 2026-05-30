# SMP Load Balancing - Quick Integration Guide

## Files Created

### Implementation
- `kernel/core/sched/scheduler_smp.c` - SMP load balancing scheduler
- `kernel/include/sched.h` - Updated header with SMP functions
- `tests/smp_load_balance_test.c` - Benchmark test

### Documentation
- `docs/SMP_LOAD_BALANCING.md` - Detailed implementation guide

## Quick Start

### 1. Build the SMP Scheduler

Add to your `kernel/core/sched/Makefile`:

```makefile
# SMP load balancing scheduler
OBJS += scheduler_smp.o
```

Or if building manually:

```bash
cd kernel/core/sched
gcc -c scheduler_smp.c -o scheduler_smp.o -I../../include
```

### 2. Link with Kernel

Ensure `scheduler_smp.o` is linked into your kernel binary.

### 3. Initialize SMP

In your `kernel_main()`:

```c
#include "sched.h"
#include "smp.h"

void kernel_main(void) {
    // ... existing initialization ...
    
    // Initialize SMP (must come first)
    smp_init();
    
    // Initialize SMP-aware scheduler
    scheduler_init();
    
    // ... rest of kernel initialization ...
}
```

### 4. Call Scheduler Tick

In your timer interrupt handler (e.g., `kernel/drivers/pit.c` or `kernel/arch/x86_64/interrupt.c`):

```c
void timer_handler(void) {
    // Update global tick counter
    ticks++;
    
    // SMP load balancing tick
    scheduler_tick();
    
    // Regular scheduling
    schedule();
}
```

### 5. Run the Benchmark

Build and link the test:

```bash
gcc -c tests/smp_load_balance_test.c -o tests/smp_load_balance_test.o -Ikernel/include
```

In your kernel test suite:

```c
#include "tests/smp_load_balance_test.c"

void run_kernel_tests(void) {
    run_smp_load_balance_test();
}
```

## Expected Behavior

### Initial State (Single CPU)
```
CPU 0: 100 processes
CPU 1: 0 processes
CPU 2: 0 processes
CPU 3: 0 processes
```

### After Load Balancing
```
CPU 0: 25 processes [PASS]
CPU 1: 25 processes [PASS]
CPU 2: 25 processes [PASS]
CPU 3: 25 processes [PASS]

Load imbalance: 0 processes
Total migrations: 75
```

## Configuration

Edit constants in `scheduler_smp.c`:

```c
// How often to balance (ticks)
#define LOAD_BALANCE_INTERVAL 50

// Minimum imbalance to trigger migration (processes)
#define LOAD_IMBALANCE_THRESHOLD 2

// Don't migrate if process ran within this many ticks
#define MIGRATION_COST_THRESHOLD 5
```

## Monitoring

### Get Load Distribution

```c
uint32_t loads[MAX_CPUS];
scheduler_get_load_stats(loads, MAX_CPUS);

for (uint32_t cpu = 0; cpu < smp_cpu_count(); cpu++) {
    kprintf("CPU %d: %d processes\n", cpu, loads[cpu]);
}
```

### Print Full Statistics

```c
scheduler_print_stats();
```

Output:
```
[SCHEDULER] Load Balancing Statistics:
  Total ticks: 5000
  Total migrations: 75
  Total work steals: 12

  Per-CPU Load:
    CPU 0: 25 processes
    CPU 1: 25 processes
    CPU 2: 25 processes
    CPU 3: 25 processes
```

## Verification

Run the benchmark and verify:

1. **✓ All processes accounted for** - Total across CPUs equals created processes
2. **✓ Load imbalance acceptable** - Difference between min/max CPUs < threshold
3. **✓ Load variance improved** - Final variance lower than initial

## Troubleshooting

### Compile Errors

**Missing `smp.h`:**
```c
#include "smp.h"  // Add to scheduler_smp.c
```

**Missing `cpu_id()` function:**

Check that `kernel/core/smp/smp.c` is compiled and linked.

**Missing `snprintf()`:**

Add to `kernel/lib/string.c`:
```c
int snprintf(char* buf, size_t size, const char* fmt, ...);
```

### Runtime Issues

**Crash on `scheduler_init()`:**

Ensure `smp_init()` is called first.

**No load balancing:**

Check that `scheduler_tick()` is called from timer handler.

**Uneven distribution:**

Increase `LOAD_BALANCE_INTERVAL` or reduce `LOAD_IMBALANCE_THRESHOLD`.

## Performance Tuning

### For I/O-Bound Workloads

```c
#define LOAD_BALANCE_INTERVAL 20     // More frequent balancing
#define LOAD_IMBALANCE_THRESHOLD 1   // Stricter threshold
```

### For CPU-Bound Workloads

```c
#define LOAD_BALANCE_INTERVAL 100    // Less frequent (reduce overhead)
#define LOAD_IMBALANCE_THRESHOLD 4   // Looser threshold
```

### For Mixed Workloads

```c
#define LOAD_BALANCE_INTERVAL 50     // Default
#define LOAD_IMBALANCE_THRESHOLD 2   // Default
```

## Next Steps

1. Run benchmark to verify correct distribution
2. Measure lock contention with multiple CPUs
3. Profile migration overhead
4. Tune constants for your workload
5. Add NUMA awareness (future enhancement)

## Support

See `docs/SMP_LOAD_BALANCING.md` for detailed documentation.

For questions or issues, refer to:
- Implementation details: `scheduler_smp.c`
- Benchmark code: `tests/smp_load_balance_test.c`
- SMP infrastructure: `kernel/include/smp.h`
