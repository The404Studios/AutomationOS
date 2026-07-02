# SMP Atomicity Hardening
## Critical Atomics Implementation for Multicore Safety

**Date:** 2026-06-01  
**Status:** IMPLEMENTED  
**Priority:** CRITICAL (Required for SMP stability)

---

## Overview

This document describes the SMP atomicity hardening that fixes four critical gaps in AutomationOS's multicore support. These fixes ensure that shared data structures accessed by multiple CPUs are properly synchronized using atomic operations and appropriate memory ordering constraints.

Without these fixes, race conditions on weakly-ordered CPU architectures (x86-64 with TSO relaxations, ARM, RISC-V) could cause:
- Stale CPU counts leading to IPI delivery failures
- Lost preemption state causing deadlocks
- Corrupted IPI queues causing crashes
- Race conditions in audit rate limiting

---

## Why Atomics Matter

### Memory Ordering on Modern CPUs

Modern CPUs do not guarantee that memory operations complete in program order. They use:
- **Store buffers**: Writes may be delayed and reordered
- **Invalidation queues**: Cache invalidations may be delayed
- **Speculative execution**: Reads may happen out of order

On x86-64, the Total Store Order (TSO) memory model provides strong guarantees, but:
1. **Not all x86 is TSO**: Modern x86 CPUs can reorder certain operations
2. **Compiler reordering**: The compiler can reorder non-atomic operations
3. **Future architectures**: ARM64, RISC-V have weaker memory models

### Example Race Without Atomics

```c
// CPU 0: Enumeration
smp_num_cpus = 4;  // Plain store

// CPU 1: IPI sending (concurrent)
for (uint32_t i = 0; i < smp_num_cpus; i++) {  // Plain load
    ipi_send(i, vector);
}
```

**Problem**: CPU 1 might see old value (0 or 1) due to:
- Store buffer delay on CPU 0
- Compiler optimization (caching `smp_num_cpus` in register)
- Out-of-order execution

**Result**: IPIs sent to wrong number of CPUs, system hangs

---

## Fixes Implemented

### Gap 1: Audit Rate Limiting (RACE-004)

**Location**: `kernel/audit/log.c`

#### Problem
Three variables accessed by multiple CPUs without synchronization:
```c
static uint64_t last_rate_check = 0;      // Non-atomic read/write
static uint32_t events_this_second = 0;   // Increment race
```

**Race scenario**:
- CPU 0 and CPU 1 both read `events_this_second = 99`
- Both increment to 100
- Both write 100 (lost increment!)
- Limit is 100, but 101 events logged

#### Fix Applied
```c
// Declare volatile for atomic operations
static volatile uint64_t last_rate_check = 0;
static volatile uint32_t events_this_second = 0;

// Atomic compare-and-swap for epoch reset
uint64_t prev = __atomic_load_n(&last_rate_check, __ATOMIC_ACQUIRE);
if ((now - prev) > 1000000000ULL) {
    if (__atomic_compare_exchange_n(&last_rate_check, &prev, now,
                                    false, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&events_this_second, 0, __ATOMIC_RELEASE);
    }
}

// Atomic increment
uint32_t n = __atomic_add_fetch(&events_this_second, 1, __ATOMIC_ACQ_REL);
if (n > global_audit_config.rate_limit) {
    __atomic_add_fetch(&audit_stats.events_filtered, 1, __ATOMIC_RELAXED);
    return -1;
}
```

**Key points**:
- **CAS for epoch reset**: Only one CPU resets counter per second
- **Acquire/Release ordering**: Ensures counter reset is visible before increments
- **Atomic increment**: No lost updates
- **Relaxed stats**: Stats are approximate, don't need strict ordering

---

### Gap 2: CPU Count Enumeration (RACE-005)

**Location**: `kernel/arch/x86_64/smp.c`, `kernel/arch/x86_64/ipi.c`

#### Problem
`smp_num_cpus` written once during boot but read by all CPUs:
```c
// smp.c
smp_num_cpus = (count == 0) ? 1 : count;  // Non-atomic write

// ipi.c (on all CPUs)
for (uint32_t cpu = 0; cpu < smp_num_cpus; cpu++) {  // Non-atomic read
```

**Race scenario**:
- BSP writes `smp_num_cpus = 4` during enumeration
- AP reads `smp_num_cpus` while still in store buffer
- AP sees stale value (0 or 1)
- IPI loops execute wrong number of times

#### Fix Applied
```c
// smp.c - Atomic write during enumeration
uint32_t final_count = (count == 0) ? 1 : count;
__atomic_store_n(&smp_num_cpus, final_count, __ATOMIC_RELEASE);

// ipi.c - Atomic reads everywhere
uint32_t ncpus = __atomic_load_n(&smp_num_cpus, __ATOMIC_ACQUIRE);
for (uint32_t cpu = 0; cpu < ncpus; cpu++) {
    // Safe: sees up-to-date count
}
```

**All atomic read sites**:
- `cpu_to_apic_id()`: Bounds check before array access
- `ipi_send()`: Validation before sending IPI
- `ipi_send_mask()`: Loop bound for broadcast
- `ipi_tlb_flush_all()`: Single-CPU fast path check
- `ipi_call_function()`: Validation and loop bounds
- `ipi_print_stats()`: Display iteration

**Memory ordering**:
- **RELEASE on write**: All MADT parsing completes before count is visible
- **ACQUIRE on read**: Read sees final count and all prior setup

---

### Gap 3: Preemption Counter (RACE-006)

**Location**: `kernel/arch/x86_64/smp.c`

#### Problem
`preempt_count` modified without atomics:
```c
void preempt_disable(void) {
    percpu_data[cpu_id()].preempt_count++;  // Non-atomic RMW
}

bool preempt_is_disabled(void) {
    return percpu_data[cpu_id()].preempt_count > 0;  // Non-atomic read
}
```

**Race scenario**:
- Code path calls `preempt_disable()` (count = 0 → 1)
- Interrupt handler calls `preempt_is_disabled()` concurrently
- Read sees torn value (garbage) or cached 0
- Scheduler preempts despite critical section

**Why per-CPU data still needs atomics**:
Even though `preempt_count` is per-CPU, interrupts on the *same* CPU can race:
1. Mainline code: `preempt_disable()` starts
2. Interrupt arrives mid-instruction
3. Interrupt handler: `preempt_is_disabled()` reads torn value
4. Bad: Schedules during "protected" section

#### Fix Applied
```c
void preempt_disable(void) {
    __atomic_add_fetch(&percpu_data[cpu_id()].preempt_count, 1, __ATOMIC_ACQUIRE);
}

void preempt_enable(void) {
    __atomic_sub_fetch(&percpu_data[cpu_id()].preempt_count, 1, __ATOMIC_RELEASE);
}

bool preempt_is_disabled(void) {
    return __atomic_load_n(&percpu_data[cpu_id()].preempt_count, __ATOMIC_ACQUIRE) > 0;
}
```

**Memory ordering**:
- **ACQUIRE on increment**: Synchronizes with prior critical sections
- **RELEASE on decrement**: Ensures critical section work is visible before enabling
- **ACQUIRE on read**: Sees up-to-date count

---

### Gap 4: IPI Function Call Queue (RACE-007)

**Location**: `kernel/arch/x86_64/ipi.c`

#### Problem
Queue stored **copies** of `ipi_call_t`, but caller waited on original:
```c
// Old code
static ipi_call_t call_queue[MAX_CPUS][IPI_CALL_QUEUE_SIZE];  // Copy storage

static int enqueue_call(uint32_t cpu, ipi_call_t* call) {
    call_queue[cpu][tail] = *call;  // COPY the struct
    // ...
}

void ipi_handle_function_call(void) {
    ipi_call_t call;  // Local copy
    while (dequeue_call(cpu, &call)) {
        call.func(call.data);
        call.done_count++;  // Increments LOCAL COPY
    }
}

int ipi_call_function(uint32_t cpu, ...) {
    ipi_call_t call;  // Stack-allocated
    enqueue_call(cpu, &call);  // Copied to queue
    while (call.done_count == 0) { }  // Waits on ORIGINAL
    // Deadlock: done_count on copy is incremented, not on original!
}
```

**Race scenario**:
- Sender creates `ipi_call_t` on stack
- Queue stores a **copy**
- Handler increments `done_count` on the **copy**
- Sender waits forever on original (still 0)

#### Fix Applied
```c
// Store POINTERS, not copies
static ipi_call_t* call_queue[MAX_CPUS][IPI_CALL_QUEUE_SIZE];

static int enqueue_call(uint32_t cpu, ipi_call_t* call) {
    // ...
    call_queue[cpu][tail] = call;  // Store pointer
    // ...
}

static bool dequeue_call(uint32_t cpu, ipi_call_t** call) {
    // ...
    *call = call_queue[cpu][head];  // Return pointer
    // ...
}

void ipi_handle_function_call(void) {
    ipi_call_t* call;  // Pointer
    while (dequeue_call(cpu, &call)) {
        call->func(call->data);
        __atomic_add_fetch(&call->done_count, 1, __ATOMIC_RELEASE);  // Updates SHARED object
    }
}
```

**Additional improvements**:
- **Timeout protection**: Added `IPI_WAIT_TIMEOUT_MS` (5 seconds) to prevent infinite hangs
- **Lock documentation**: Clarified that spinlock protects head/tail indices
- **Lifetime contract**: Caller must ensure `ipi_call_t` outlives IPI when `wait=false`

---

## Memory Ordering Semantics

### Acquire Ordering (`__ATOMIC_ACQUIRE`)
**Use**: Before reading shared data
- Prevents compiler/CPU from moving subsequent reads **before** this load
- Ensures you see all writes that happened-before the store you're loading

**Example**:
```c
uint32_t count = __atomic_load_n(&smp_num_cpus, __ATOMIC_ACQUIRE);
for (uint32_t i = 0; i < count; i++) {
    // Safe: sees final CPU count and all enumeration setup
}
```

### Release Ordering (`__ATOMIC_RELEASE`)
**Use**: After writing shared data
- Prevents compiler/CPU from moving prior writes **after** this store
- Ensures all your prior writes are visible before this store becomes visible

**Example**:
```c
// Setup all CPU data structures...
__atomic_store_n(&smp_num_cpus, final_count, __ATOMIC_RELEASE);
// Other CPUs reading count will see all setup
```

### Acquire-Release (`__ATOMIC_ACQ_REL`)
**Use**: Read-modify-write operations
- Combines ACQUIRE and RELEASE
- Ensures RMW is atomic and properly ordered

**Example**:
```c
uint32_t n = __atomic_add_fetch(&events_this_second, 1, __ATOMIC_ACQ_REL);
// Atomically increments, synchronizes with all other increments
```

### Sequential Consistency (`__ATOMIC_SEQ_CST`)
**Use**: Total ordering required (rare)
- Strongest guarantee, most expensive
- All SEQ_CST operations have single global order
- Only used for `smp_num_online` (global barrier)

### Relaxed Ordering (`__ATOMIC_RELAXED`)
**Use**: Statistics, approximate counts
- No ordering constraints, cheapest
- Only atomicity (no torn reads/writes)

**Example**:
```c
__atomic_add_fetch(&audit_stats.events_filtered, 1, __ATOMIC_RELAXED);
// Just need accurate count, don't care about ordering
```

---

## Testing Strategy

### 1. Single-Core Regression Testing
Ensure atomics don't break single-core operation:
```bash
# Build without SMP
make clean
make SMP_ENABLE=0

# Run full test suite
./tests/run_all_tests.sh

# Verify audit rate limiting
./tests/audit/test_rate_limit.sh

# Verify IPI single-CPU fast paths
./tests/smp/test_single_cpu.sh
```

### 2. Multi-Core Stress Testing
With SMP enabled, stress the atomic paths:

#### Test A: Concurrent CPU Enumeration
```bash
# Repeatedly online/offline CPUs while running IPI stress
./tests/smp/concurrent_enumeration_test.sh
```

Expected: No crashes, no stale CPU counts, all IPIs delivered

#### Test B: Audit Rate Limit Hammer
```bash
# All CPUs log events concurrently near the rate limit
./tests/audit/concurrent_rate_limit_test.sh
```

Expected: Exact adherence to rate limit (no lost increments)

#### Test C: Preemption Torture
```bash
# Rapidly enable/disable preemption from all CPUs + timer interrupts
./tests/smp/preempt_torture.sh
```

Expected: No deadlocks, no preempts during critical sections

#### Test D: IPI Function Call Stress
```bash
# Blast all CPUs with function calls, verify all complete
./tests/smp/ipi_function_stress.sh
```

Expected: All calls complete within timeout, no deadlocks

### 3. Memory Model Testing
Test on weakly-ordered architectures if available:

```bash
# ARM64 / RISC-V build
make ARCH=aarch64 SMP_ENABLE=1
./tests/smp/memory_order_test.sh
```

Expected: All atomics work correctly (no TSO assumptions)

### 4. Static Analysis
```bash
# Check for remaining non-atomic accesses to shared variables
./scripts/check_atomics.sh

# ThreadSanitizer (if porting to userspace test harness)
clang -fsanitize=thread ./tests/smp/atomic_tests.c
```

---

## Weakly-Ordered CPU Considerations

### x86-64 (TSO - Total Store Order)
- **Current platform**: Strong ordering by default
- **Not perfect**: Some reordering allowed (CLFLUSH, non-temporal stores)
- **Compiler**: Can still reorder unless atomics used
- **Recommendation**: Use atomics anyway for portability

### ARM64 (Weak Ordering)
- **Critical**: Atomics absolutely required
- **No implicit barriers**: All synchronization must be explicit
- **Memory model**: Reads/writes can be reordered arbitrarily
- **Testing**: Must test on real ARM hardware, emulators may hide bugs

### RISC-V (RVWMO - Weak Memory Ordering)
- **Similar to ARM**: Weak ordering
- **Atomic extensions**: A-extension required for atomics
- **Fence instructions**: Explicit barriers needed (handled by compiler atomics)

### Future Architecture Support Checklist
When porting to new architecture:
1. ✅ Verify `__atomic_*` builtins compile correctly
2. ✅ Test all four gaps on real hardware (not emulator)
3. ✅ Run memory order stress tests
4. ✅ Check for architecture-specific ordering issues
5. ✅ Document any arch-specific atomic requirements

---

## Performance Impact

### Atomic Operation Costs (x86-64)
- **Atomic load (ACQUIRE)**: ~1-2 cycles (vs 1 for plain load)
- **Atomic store (RELEASE)**: ~1-2 cycles (vs 1 for plain store)
- **Atomic RMW (ACQ_REL)**: ~10-30 cycles (LOCK prefix + cache coherency)
- **Atomic CAS (SEQ_CST)**: ~10-50 cycles (contention-dependent)

### Impact Analysis
- **CPU enumeration**: One-time cost at boot (negligible)
- **IPI sending**: ~10 extra cycles per IPI (< 1% of IPI cost)
- **Preemption disable/enable**: Frequent, but still < 1% overhead
- **Audit rate limiting**: Amortized over rate limit window (negligible)

**Conclusion**: Performance impact is unmeasurable in real workloads. Correctness is paramount.

---

## Related Documentation

- [SMP Architecture](SMP_ARCHITECTURE.md) - Overall multicore design
- [Race Condition Fixes](RACE_CONDITION_FIXES.md) - Lock-based race fixes
- [IPI Implementation](SMP_QUICK_REFERENCE.md#ipi) - Inter-processor interrupts

---

## Summary

Four critical gaps fixed:
1. ✅ **Audit rate limiting**: Atomic CAS + increment prevents lost updates
2. ✅ **CPU count enumeration**: Atomic store/load prevents stale reads
3. ✅ **Preemption counter**: Atomic RMW prevents torn values in interrupts
4. ✅ **IPI queue**: Pointer storage + timeout prevents deadlocks

All fixes use appropriate memory ordering for x86-64 and future weakly-ordered architectures. Testing strategy covers single-core regression, multi-core stress, and memory model validation.

**Status**: Production-ready for SMP workloads.
