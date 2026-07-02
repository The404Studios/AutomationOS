# SMP Audit Rate Limiter Test Suite

## Quick Start

```bash
# Build and run all tests
make test

# Run just the SMP stress test
make test-smp

# Run just the single-CPU baseline test
make test-single

# Clean build artifacts
make clean
```

## What Gets Tested

✅ **Rate limit enforcement** - No overflow beyond configured limit  
✅ **Atomic correctness** - No lost updates from race conditions  
✅ **Stability** - No crashes or deadlocks under concurrent load  
✅ **Fairness** - All CPUs get access, no starvation  

## Test Files

| File | Purpose |
|------|---------|
| `test_audit_rate_limit_smp.c` | Main SMP stress test implementation |
| `verify_audit_rate_limit.sh` | Automated build and run script |
| `AUDIT_RATE_LIMIT_SMP_TEST.md` | Comprehensive test documentation |
| `Makefile` | Build system for tests |
| `README.md` | This file |

## Test Configuration

Default settings (edit in `test_audit_rate_limit_smp.c`):

```c
#define TEST_RATE_LIMIT         1000    // Events per second
#define TEST_DURATION_MS        2000    // 2 second test
#define EVENTS_PER_CPU          5000    // Attempts per CPU
```

## Pass/Fail Criteria

### ✅ Test PASSES if:
- All CPUs finish without timeout
- `succeeded + rate_limited == attempted` (no lost updates)
- `total_logged <= rate_limit * duration + tolerance`
- All CPUs participated (finished == true)

### ❌ Test FAILS if:
- Lost updates detected (atomicity violation)
- Rate limit exceeded significantly
- CPUs timeout or hang
- Kernel panic or crash

## Example Output (PASS)

```
========================================
  OVERALL: PASSED
========================================
  [PASS] Atomicity: All updates accounted for
  [PASS] Rate limit: 1000 events (max ~2000)
  [PASS] No events lost in transit
```

## Example Output (FAIL)

```
========================================
  OVERALL: FAILED
========================================
  [FAIL] Atomicity: 70 updates lost
  [FAIL] Rate limit: 3500 events exceeds max ~2000
```

## Rate Limiter Code Under Test

From `kernel/audit/log.c` (lines 92-110):

```c
// Rate limiting with atomic operations for SMP safety
uint64_t now = audit_get_timestamp();
if (global_audit_config.rate_limit > 0) {
    uint64_t prev = __atomic_load_n(&last_rate_check, __ATOMIC_ACQUIRE);
    if ((now - prev) > 1000000000ULL) {
        // CAS: only one CPU resets counter
        if (__atomic_compare_exchange_n(&last_rate_check, &prev, now,
                                        false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&events_this_second, 0, __ATOMIC_RELEASE);
        }
    }
    
    // Atomic increment and check
    uint32_t n = __atomic_add_fetch(&events_this_second, 1, __ATOMIC_ACQ_REL);
    if (n > global_audit_config.rate_limit) {
        return -1;  // Rate limited
    }
}
```

## SMP Safety Mechanisms

1. **Atomic Load** (`__atomic_load_n`) - Read shared variable safely
2. **Compare-and-Swap** (`__atomic_compare_exchange_n`) - Only one CPU resets counter
3. **Atomic Increment** (`__atomic_add_fetch`) - Increment counter without races
4. **Memory Ordering** (`__ATOMIC_ACQ_REL`) - Ensure proper ordering across CPUs

## Current Limitations

⚠️ **Sequential Execution**: Without full SMP scheduler, workers run sequentially (not truly concurrent)  
⚠️ **Fake Timestamps**: Uses counter instead of real time  
⚠️ **No CPU Pinning**: Can't pin threads to specific CPUs yet  

These limitations don't prevent testing the rate limiter logic, but may miss some race conditions that would appear under true concurrency.

## Future Work

🔧 Integrate with SMP scheduler for true concurrent execution  
🔧 Use TSC/HPET for real timestamp-based rate limiting  
🔧 Add IPI-based cross-CPU testing  
🔧 Longer stress tests (1+ minute) for rare races  
🔧 Burst traffic patterns  

## Troubleshooting

### Build Errors

```bash
# Missing audit object files
cd ../../kernel/audit
make

# Missing SMP headers
# Check that kernel/include/smp.h exists
```

### Test Hangs

- Check for deadlocks in audit buffer (spinlock issues)
- Increase timeout in test code
- Run with debug output enabled

### Rate Limit Violations

- Verify `TEST_RATE_LIMIT` matches config
- Check timing precision (fake vs real timestamps)
- Review atomic operation implementation

## Integration with CI/CD

Add to your continuous integration:

```yaml
# .github/workflows/test.yml
- name: Run SMP Audit Tests
  run: |
    cd tests/smp
    make test
```

## Documentation

See `AUDIT_RATE_LIMIT_SMP_TEST.md` for:
- Detailed test methodology
- Rate limiter implementation details
- Verification check explanations
- Expected output examples
- Failure interpretation guide

## Contact

**Project**: AutomationOS  
**Repository**: https://github.com/The404Studios/AutomationOS  
**Email**: the404studios@gmail.com  

## License

Same as AutomationOS kernel (see repository root LICENSE file)
