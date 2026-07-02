# SMP Stress Test for Audit Rate Limiter

## Overview

This test validates the correctness and robustness of the audit subsystem's rate limiter under heavy concurrent load from multiple CPUs. The audit rate limiter uses atomic operations (`__atomic_*` builtins) to enforce a maximum events-per-second limit while maintaining correctness across SMP systems.

## What This Tests

### 1. Rate Limit Enforcement
- **Goal**: Verify that the total number of logged events does not exceed the configured rate limit
- **Method**: Configure a rate limit (e.g., 1000 events/sec), have all CPUs hammer `audit_log()` simultaneously, count total logged events
- **Pass Criteria**: `total_logged <= rate_limit * duration + tolerance`

### 2. Atomic Counter Correctness (No Lost Updates)
- **Goal**: Verify that all updates to shared counters are atomic and no updates are lost due to race conditions
- **Method**: Each CPU tracks how many times it called `audit_log()` and what the return value was. Sum across all CPUs and compare to audit subsystem statistics.
- **Pass Criteria**: `sum(succeeded) + sum(rate_limited) == sum(attempted)` (no lost updates)

### 3. No Crashes or Deadlocks
- **Goal**: System remains stable under concurrent load
- **Method**: All CPUs complete their work within a reasonable timeout
- **Pass Criteria**: All CPUs finish, no hangs, no kernel panics

### 4. Fair Access Across CPUs
- **Goal**: No CPU is starved; all CPUs get a chance to log events
- **Method**: Verify that all CPUs successfully logged at least some events
- **Pass Criteria**: All CPUs show non-zero succeeded count (or reasonable distribution)

## Rate Limiter Implementation

The rate limiter in `kernel/audit/log.c` (lines 92-110) uses atomic operations for SMP safety:

```c
// Check rate limit
uint64_t now = audit_get_timestamp();
if (global_audit_config.rate_limit > 0) {
    uint64_t prev = __atomic_load_n(&last_rate_check, __ATOMIC_ACQUIRE);
    if ((now - prev) > 1000000000ULL) {  // 1 second
        // Only the CPU that wins the CAS resets the counter; others skip.
        if (__atomic_compare_exchange_n(&last_rate_check, &prev, now,
                                        false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&events_this_second, 0, __ATOMIC_RELEASE);
        }
    }

    uint32_t n = __atomic_add_fetch(&events_this_second, 1, __ATOMIC_ACQ_REL);
    if (n > global_audit_config.rate_limit) {
        __atomic_add_fetch(&audit_stats.events_filtered, 1, __ATOMIC_RELAXED);
        return -1;  // Rate limit exceeded
    }
}
```

### Key SMP Safety Mechanisms

1. **Atomic Load/Store**: `__atomic_load_n` and `__atomic_store_n` ensure visibility across CPUs
2. **Compare-and-Swap (CAS)**: Only one CPU resets the counter per second (winner of the race)
3. **Atomic Increment**: `__atomic_add_fetch` increments the counter atomically
4. **Memory Ordering**: `__ATOMIC_ACQ_REL` ensures proper ordering of operations across CPUs

## Test Configuration

| Parameter | Default | Description |
|-----------|---------|-------------|
| `TEST_RATE_LIMIT` | 1000 | Events per second limit |
| `TEST_DURATION_MS` | 2000 | Test duration (2 seconds) |
| `EVENTS_PER_CPU` | 5000 | How many times each CPU calls `audit_log()` |
| `MAX_CPUS` | 256 | Maximum CPUs supported |

## Test Workflow

```
1. Initialize audit subsystem
   └─> audit_init()
   └─> Configure rate_limit = 1000 events/sec
   └─> Reset statistics

2. Launch worker on each CPU
   ├─> CPU 0: audit_stress_worker(0)
   ├─> CPU 1: audit_stress_worker(1)
   ├─> CPU 2: audit_stress_worker(2)
   └─> ...

3. Workers execute concurrently
   ├─> Each CPU calls audit_log() 5000 times as fast as possible
   ├─> Track: attempts, successes (ret=0), rate_limited (ret=-1)
   └─> Small yield every 100 iterations to allow contention

4. Wait for all CPUs to finish
   └─> Timeout = 5 seconds

5. Verify results
   ├─> Aggregate per-CPU statistics
   ├─> Compare to audit subsystem statistics
   ├─> Check for lost updates (atomicity violation)
   ├─> Check rate limit enforcement
   └─> Generate pass/fail verdict

6. Print detailed report
```

## Per-CPU Test State

Each CPU maintains its own state to avoid false sharing:

```c
typedef struct {
    uint32_t cpu_id;
    uint64_t events_attempted;      // How many calls to audit_log()
    uint64_t events_succeeded;      // How many returned 0
    uint64_t events_rate_limited;   // How many returned -1
    bool started;
    bool finished;
} cpu_test_state_t;
```

## Verification Checks

### Check 1: Atomicity (No Lost Updates)

```
total_attempted = sum(cpu_states[i].events_attempted for all CPUs)
total_accounted = sum(cpu_states[i].events_succeeded + 
                      cpu_states[i].events_rate_limited for all CPUs)

PASS if: total_accounted == total_attempted
FAIL if: total_accounted < total_attempted (lost updates)
```

**What it detects**: If the atomic operations are not working correctly, some return values from `audit_log()` may be lost or corrupted, leading to a mismatch.

### Check 2: Rate Limit Enforcement

```
expected_max = rate_limit * (duration_seconds + 1) + tolerance

PASS if: audit_total_events <= expected_max
FAIL if: audit_total_events > expected_max
```

**What it detects**: If the rate limiter logic is broken, the system may log more events than configured. The tolerance accounts for timing skew.

### Check 3: All CPUs Participated

```
active_cpus = count(cpu_states[i].finished == true)

PASS if: active_cpus == smp_num_cpus
FAIL if: active_cpus < smp_num_cpus
```

**What it detects**: If a CPU hangs, deadlocks, or is starved, it won't finish.

## Expected Output

### Successful Test Run

```
========================================
SMP Audit Rate Limiter Stress Test
========================================
Configuration:
  CPUs:                4
  Rate limit:          1000 events/sec
  Events per CPU:      5000
  Total attempts:      20000

Per-CPU Results:
  CPU  0: attempted= 5000  succeeded=  250  rate_limited= 4750  DONE
  CPU  1: attempted= 5000  succeeded=  255  rate_limited= 4745  DONE
  CPU  2: attempted= 5000  succeeded=  245  rate_limited= 4755  DONE
  CPU  3: attempted= 5000  succeeded=  250  rate_limited= 4750  DONE

Aggregate Results:
  Total attempted:     20000
  Total succeeded:     1000
  Total rate limited:  19000
  Lost updates:        0 [OK]

Audit Subsystem Stats:
  Total events logged: 1000
  Events filtered:     19000

Verification:
  [PASS] Atomicity: All updates accounted for
  [PASS] Rate limit: 1000 events (max ~2000)
  [PASS] No events lost in transit

========================================
  OVERALL: PASSED
========================================
```

### Failed Test (Lost Updates)

```
Aggregate Results:
  Total attempted:     20000
  Total succeeded:     980
  Total rate limited:  18950
  Lost updates:        70 [FAIL]

Verification:
  [FAIL] Atomicity: 70 updates lost
```

**Cause**: Atomic operations not working correctly, race conditions in counter updates.

### Failed Test (Rate Limit Violated)

```
Audit Subsystem Stats:
  Total events logged: 3500
  Events filtered:     16500

Verification:
  [FAIL] Rate limit: 3500 events exceeds max ~2000
```

**Cause**: Rate limiter logic broken, not enforcing limit correctly.

## Building and Running

### Method 1: Use Verification Script

```bash
cd tests/smp
chmod +x verify_audit_rate_limit.sh
./verify_audit_rate_limit.sh
```

### Method 2: Manual Build

```bash
# Compile test
gcc -O2 \
    -I../../kernel/include \
    -DSMP_ENABLE \
    -c test_audit_rate_limit_smp.c \
    -o test_audit_rate_limit_smp.o

# Link with audit subsystem
gcc -O2 \
    test_audit_rate_limit_smp.o \
    ../../kernel/audit/log.o \
    ../../kernel/audit/buffer.o \
    ../../kernel/audit/filter.o \
    ../../kernel/audit/rules.o \
    -o test_audit_rate_limit_smp

# Run test
./test_audit_rate_limit_smp
```

### Method 3: Integration with Kernel Test Suite

Add to `tests/Makefile`:

```make
test-smp-audit: $(BUILD_DIR)/test_audit_rate_limit_smp
	@echo "Running SMP audit rate limiter test..."
	@$(BUILD_DIR)/test_audit_rate_limit_smp
```

## Running in QEMU/Real Hardware

For testing on actual SMP hardware or QEMU with multiple CPUs:

```bash
# QEMU with 4 CPUs
qemu-system-x86_64 \
    -smp cpus=4 \
    -kernel kernel.bin \
    -append "test=audit_rate_limit_smp"
```

The kernel's test runner should invoke `test_audit_rate_limit_smp()` at boot.

## Limitations (Current Implementation)

### 1. Simulated Multi-CPU Test
Without full SMP scheduler support, the test currently runs workers **sequentially** rather than concurrently on different CPUs. This still tests the rate limiter logic but doesn't expose true race conditions.

**Workaround**: Enable `SMP_ENABLE` and use IPI-based scheduling once per-CPU runqueues are implemented.

### 2. Timing Precision
The test uses a simple counter-based timestamp (`audit_get_timestamp()`) which increments on every call. This doesn't represent real time, so rate limiting is based on event count rather than actual time.

**Workaround**: Integrate with TSC or HPET timer for real nanosecond timestamps.

### 3. No Per-CPU Scheduling
The current kernel doesn't support pinning threads to specific CPUs, so we can't truly launch a worker on each CPU simultaneously.

**Workaround**: Use `smp_call_function_single()` or similar IPI mechanism (requires future SMP scheduler work).

## Future Enhancements

1. **True Concurrent Execution**: Integrate with SMP scheduler to run workers on separate CPUs simultaneously
2. **Real-Time Measurement**: Use TSC/HPET for actual time-based rate limiting tests
3. **Burst Testing**: Test rate limiter behavior with bursty traffic (e.g., all CPUs log 1000 events instantly)
4. **Dynamic Rate Limit Changes**: Test changing rate limit while test is running
5. **Stress Duration**: Longer tests (e.g., 1 minute) to detect rare race conditions
6. **Fault Injection**: Simulate corrupted atomic operations to verify detection

## Interpreting Failures

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| Lost updates > 0 | Atomic operations not working | Check compiler flags, verify atomics |
| Rate limit violated | Rate limiter logic bug | Review `audit_log()` rate limit code |
| Timeout (CPUs don't finish) | Deadlock or infinite loop | Check for spinlock contention |
| Uneven CPU distribution | Unfair lock acquisition | Review lock implementation |
| Kernel panic | Memory corruption, race condition | Enable KASAN, review shared state |

## Related Files

- **Implementation**: `kernel/audit/log.c` (lines 92-110) - Rate limiter logic
- **Header**: `kernel/include/audit.h` - Audit structures and API
- **Buffer**: `kernel/audit/buffer.c` - Ring buffer (also SMP-safe with spinlocks)
- **Stats**: `kernel/audit/log.c` - Atomic statistics counters
- **SMP**: `kernel/arch/x86_64/smp.c` - SMP infrastructure

## References

- [GCC Atomic Builtins](https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html)
- [C11 Memory Model](https://en.cppreference.com/w/c/atomic/memory_order)
- [Linux Kernel Atomics](https://www.kernel.org/doc/html/latest/core-api/atomic_ops.html)
- [SMP Bring-up Discipline](../../.claude/projects/*/memory/feedback_smp_bringup_discipline.md)

## Contact

For questions or issues with this test:
- **Email**: the404studios@gmail.com
- **Repository**: [AutomationOS](https://github.com/The404Studios/AutomationOS)
