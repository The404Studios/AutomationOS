# Scheduler Fairness Test Plan

## Overview
This document describes how to test and verify that the scheduler time slice bug fix is working correctly.

## Test Setup

### Test Environment
- Kernel with fixed scheduler (kernel/core/sched/scheduler.c)
- Timer frequency: 100 Hz (10ms per tick)
- Time slice quantum: 10 ticks (100ms per quantum)

### Prerequisites
```bash
# Build kernel with tests
make clean
make all

# Or run tests directly
make test
```

## Unit Tests

### Test 1: Time Slice Preservation Test

**File:** `tests/unit/test_scheduler.c::test_scheduler_time_slice_fairness()`

**Purpose:** Verify that preempted processes maintain their remaining time slice

**Steps:**
1. Create process A with fresh time slice (10 ticks)
2. Simulate 5 ticks of execution (time_slice = 5 remaining)
3. Preempt process A (add back to ready queue)
4. Verify time_slice is still 5 (NOT reset to 10)
5. Re-schedule process A
6. Verify it resumes with 5 ticks (NOT 10)

**Expected Output:**
```
[TEST] Process A picked with time_slice = 10 (expected 10) ✓
[TEST] Simulating 5 ticks elapsed, time_slice now = 5
[TEST] Process A re-added to queue, time_slice = 5 (should still be 5) ✓
[TEST] Process A re-picked, time_slice = 5 (should still be 5, NOT 10!) ✓
```

**Pass Criteria:** All assertions pass ✓

### Test 2: CPU Time Fairness Test

**File:** `tests/unit/test_scheduler.c::test_scheduler_cpu_fairness()`

**Purpose:** Verify that 3 CPU-bound processes get equal CPU time

**Steps:**
1. Create 3 identical CPU-bound processes (A, B, C)
2. Add all to ready queue
3. Simulate 300 timer ticks (30 context switches)
4. Track CPU time for each process
5. Verify each got ~100 ticks (33.3% ± 3%)

**Expected Output:**
```
[TEST] CPU time distribution over 300 ticks:
[TEST]   Process A: 100 ticks (33.3%) ✓
[TEST]   Process B: 100 ticks (33.3%) ✓
[TEST]   Process C: 100 ticks (33.3%) ✓
```

**Pass Criteria:** Each process gets 90-110 ticks ✓

## Integration Tests

### Test 3: Mixed Workload Test (CPU + I/O)

**Purpose:** Verify fairness with mixed CPU-bound and I/O-bound processes

**Setup:**
```c
// CPU-bound process (uses full quantum)
void cpu_bound_task(void) {
    while (1) {
        // Busy wait
        for (volatile int i = 0; i < 1000000; i++);
    }
}

// I/O-bound process (yields after 3 ticks)
void io_bound_task(void) {
    while (1) {
        // Use 3 ticks of CPU
        for (volatile int i = 0; i < 300000; i++);
        
        // Simulate I/O (sleep)
        sleep(50);  // 50ms sleep
    }
}
```

**Expected Behavior:**
1. CPU-bound process:
   - Gets 10 ticks per quantum
   - Preempted when time_slice = 0
   - Gets fresh 10 ticks on next schedule

2. I/O-bound process:
   - Uses 3 ticks, then blocks on I/O
   - Maintains time_slice = 7 while blocked
   - Resumes with 7 ticks after I/O completes
   - After 2 resumes, exhausts quantum → gets fresh 10 ticks

**Expected Scheduler Logs:**
```
[SCHEDULER] Fresh quantum for 'cpu_bound' (PID 2): 10 ticks
[SCHEDULER] Time slice expired for process 'cpu_bound' (PID 2) - preempting
[SCHEDULER] Fresh quantum for 'io_bound' (PID 3): 10 ticks
... (3 ticks later)
[SCHEDULER] Resuming 'io_bound' (PID 3) with 7 ticks remaining  ← KEY!
... (I/O completes)
[SCHEDULER] Resuming 'io_bound' (PID 3) with 7 ticks remaining  ← KEY!
```

**Pass Criteria:**
- I/O-bound process shows "Resuming with X ticks remaining" (NOT "Fresh quantum")
- CPU-bound process always shows "Fresh quantum" after preemption
- Both processes make progress (no starvation)

### Test 4: Stress Test (Many Processes)

**Purpose:** Verify fairness with many concurrent processes

**Setup:**
```c
// Create 10 CPU-bound processes
for (int i = 0; i < 10; i++) {
    char name[16];
    snprintf(name, sizeof(name), "cpu_%d", i);
    process_create(name, cpu_bound_task);
}
```

**Run for:** 10000 timer ticks (1000 context switches)

**Expected Distribution:**
Each process should get approximately 1000 ticks (10% ± 2%)

**Pass Criteria:**
- Each process: 980-1020 ticks ✓
- No process starvation ✓
- Context switches: ~1000 ✓

## Manual Testing

### Test 5: Visual Fairness Test

**Purpose:** Visually verify fair scheduling with progress bars

**Setup:**
```c
void progress_task(int id) {
    uint64_t work = 0;
    while (1) {
        work++;
        if (work % 1000000 == 0) {
            kprintf("Process %d: progress = %llu\n", id, work / 1000000);
        }
    }
}

// Create 3 progress tasks
for (int i = 0; i < 3; i++) {
    process_create("progress", progress_task, i);
}
```

**Expected Output (after ~5 seconds):**
```
Process 0: progress = 50
Process 1: progress = 50
Process 2: progress = 50
Process 0: progress = 100
Process 1: progress = 100
Process 2: progress = 100
```

**Pass Criteria:**
- All processes have similar progress values (±10%) ✓
- No process is significantly behind ✓

## Performance Tests

### Test 6: Context Switch Overhead

**Purpose:** Verify that the fix doesn't add significant overhead

**File:** `tests/bench/bench_context_switch.c`

**Measurement:**
```c
uint64_t start = rdtsc();
for (int i = 0; i < 10000; i++) {
    schedule();  // Force context switch
}
uint64_t end = rdtsc();
uint64_t cycles_per_switch = (end - start) / 10000;
```

**Expected:** < 5000 cycles per context switch on modern hardware

**Pass Criteria:** No regression from baseline ✓

## Debugging Failed Tests

### Symptom: Process gets "Fresh quantum" when it should resume with remaining time

**Root Cause:** time_slice is being reset somewhere it shouldn't be

**Check:**
1. `scheduler_add_process()` - Should NOT modify time_slice
2. `scheduler_pick_next()` - Should only reset if time_slice == 0
3. Process creation - Should initialize time_slice = 0

### Symptom: Unequal CPU time distribution

**Root Cause:** Time slice not being decremented correctly

**Check:**
1. Timer handler calls `schedule()` every tick ✓
2. `schedule()` decrements `current->time_slice--` ✓
3. Preemption triggers when `time_slice == 0` ✓

### Symptom: Process starvation

**Root Cause:** Queue management issue (not related to time slice bug)

**Check:**
1. `scheduler_add_process()` adds to tail ✓
2. `scheduler_pick_next()` picks from head ✓
3. No process stays in queue indefinitely ✓

## Automated Test Suite

### Running All Tests

```bash
# Build and run all unit tests
make test-unit

# Build and run integration tests
make test-integration

# Build and run all tests
make test-all
```

### Expected Output

```
=====================================
   Running Scheduler Tests          
=====================================

[TEST] Testing scheduler_init...
[TEST] scheduler_init: PASS

[TEST] Testing scheduler_add_process...
[TEST] scheduler_add_process: PASS

[TEST] Testing round-robin scheduling...
[TEST] round-robin scheduling: PASS

[TEST] Testing scheduler_remove_process...
[TEST] scheduler_remove_process: PASS

--- TIME SLICE BUG FIX VERIFICATION ---

[TEST] Testing time slice fairness (preemption bug fix)...
[TEST] time_slice fairness: PASS - Preempted processes correctly keep remaining time!

[TEST] Testing CPU time fairness over multiple scheduling rounds...
[TEST] CPU fairness: PASS - Each process got fair share!

=====================================
   All Scheduler Tests Passed       
=====================================
```

## Continuous Integration

### CI Pipeline

```yaml
# .github/workflows/scheduler-tests.yml
name: Scheduler Fairness Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      - name: Build kernel
        run: make kernel
      - name: Run scheduler unit tests
        run: make test-unit
      - name: Run scheduler integration tests
        run: make test-integration
      - name: Verify no regression
        run: make bench-context-switch
```

## Conclusion

This test plan ensures comprehensive verification of the scheduler time slice bug fix across multiple scenarios:

- ✓ Unit tests verify individual components
- ✓ Integration tests verify system behavior
- ✓ Manual tests verify visual correctness
- ✓ Performance tests verify no regression
- ✓ Stress tests verify scalability

All tests should pass with the fixed scheduler implementation.

---
**Test Plan Version:** 1.0  
**Last Updated:** 2026-05-26  
**Maintainer:** Kernel Team  
