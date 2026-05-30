# Race Condition Fixes Implementation Guide
## AutomationOS SMP Safety Patches

**Date:** 2026-05-26  
**Status:** Ready for Implementation  
**Priority:** CRITICAL (Blocking SMP support)

---

## Overview

This document provides step-by-step instructions to fix all critical race conditions identified in the audit. All fixes use existing spinlock infrastructure and maintain compatibility with single-core operation.

---

## Fix 1: Scheduler Lock (RACE-001) - IMPLEMENTED ✅

### Location
`kernel/core/sched/scheduler.c`

### Changes Made
1. Added `#include "../../include/spinlock.h"`
2. Added global `static spinlock_t scheduler_lock;`
3. Initialize lock in `scheduler_init()`
4. Protected all queue operations with lock

### Code
```c
// At top of file
#include "../../include/spinlock.h"

static spinlock_t scheduler_lock;

// In scheduler_init()
void scheduler_init(void) {
    spin_lock_init(&scheduler_lock);
    // ... existing code ...
}

// In scheduler_add_process()
void scheduler_add_process(process_t* proc) {
    // ... validation ...
    process_ref(proc);
    
    spin_lock(&scheduler_lock);
    // ... queue manipulation ...
    spin_unlock(&scheduler_lock);
}

// In scheduler_remove_process()
void scheduler_remove_process(process_t* proc) {
    spin_lock(&scheduler_lock);
    // ... queue manipulation ...
    spin_unlock(&scheduler_lock);
}

// In scheduler_pick_next()
process_t* scheduler_pick_next(void) {
    spin_lock(&scheduler_lock);
    // ... queue access ...
    spin_unlock(&scheduler_lock);
    return next;
}
```

### Testing
```bash
# Single-core (should work as before)
make test

# Multi-core stress test (when SMP enabled)
./tests/stress/scheduler_concurrent_test
```

---

## Fix 2: Process Table Lock (RACE-002) - TODO

### Location
`kernel/core/sched/process.c`

### Required Changes

#### Step 1: Add Lock and Headers
```c
#include "../../include/spinlock.h"

// After process_table declaration
static spinlock_t process_table_lock;
```

#### Step 2: Initialize Lock
```c
void process_init(void) {
    kprintf("[PROCESS] Initializing process management...\n");
    
    spin_lock_init(&process_table_lock);  // ADD THIS
    
    // Clear process table
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_table[i] = NULL;
    }
    
    kprintf("[PROCESS] Process table initialized (max %d processes, SMP-safe)\n", 
            MAX_PROCESSES);
}
```

#### Step 3: Protect process_create() Table Access
```c
process_t* process_create(const char* name, void* entry_point) {
    // ... existing allocation code ...
    
    proc->sandbox_flags = 0;
    
    // Add to process table (PROTECTED)
    spin_lock(&process_table_lock);
    process_table[pid] = proc;
    spin_unlock(&process_table_lock);
    
    kprintf("[PROCESS] Created process '%s' (PID %d) at entry %p\n",
            proc->name, proc->pid, entry_point);
    
    return proc;
}
```

#### Step 4: Protect process_unref() Table Access
```c
void process_unref(process_t* proc) {
    // ... existing ref count decrement ...
    
    if (old_count == 0) {
        kprintf("[PROCESS] Freeing process '%s' (PID %d) - ref_count reached 0\n",
                proc->name, proc->pid);
        
        // Remove from process table (PROTECTED)
        spin_lock(&process_table_lock);
        if (proc->pid < MAX_PROCESSES) {
            process_table[proc->pid] = NULL;
        }
        spin_unlock(&process_table_lock);
        
        // Free kernel stack
        // ... rest of cleanup ...
    }
}
```

#### Step 5: Protect process_get_by_pid()
```c
process_t* process_get_by_pid(uint32_t pid) {
    if (pid >= MAX_PROCESSES) {
        return NULL;
    }
    
    // CRITICAL: Read with lock AND take reference
    spin_lock(&process_table_lock);
    process_t* proc = process_table[pid];
    if (proc) {
        process_ref(proc);  // Increment before returning
    }
    spin_unlock(&process_table_lock);
    
    return proc;
}
```

**IMPORTANT:** Callers of `process_get_by_pid()` must now call `process_unref()` when done!

### API Change Impact
Before:
```c
process_t* proc = process_get_by_pid(5);
// Use proc
```

After:
```c
process_t* proc = process_get_by_pid(5);
// Use proc
process_unref(proc);  // MUST RELEASE REFERENCE
```

---

## Fix 3: Atomic PID Allocation (RACE-003) - TODO

### Location
`kernel/core/sched/process.c`

### Current Code (WRONG)
```c
static uint32_t allocate_pid(void) {
    if (next_pid >= MAX_PROCESSES) {
        kernel_panic("Process table full");
    }
    return next_pid++;  // RACE CONDITION!
}
```

### Fixed Code (CORRECT)
```c
static uint32_t allocate_pid(void) {
    uint32_t pid = __atomic_fetch_add(&next_pid, 1, __ATOMIC_SEQ_CST);
    if (pid >= MAX_PROCESSES) {
        kernel_panic("Process table full");
    }
    return pid;
}
```

### Why This Works
- `__atomic_fetch_add()` returns OLD value and increments atomically
- Multiple CPUs cannot get the same PID
- No lock needed (lock-free!)

### Note on PID Reuse
The `free_pid()` function needs updating:
```c
static void free_pid(uint32_t pid) {
    // Atomic increment makes PID reuse complex
    // For now, just log freed PIDs
    // TODO: Implement free list for PID reuse
    kprintf("[PROCESS] PID %d freed (reuse requires free list)\n", pid);
}
```

---

## Fix 4: IRQ Descriptor Status (RACE-004) - TODO

### Location
`kernel/drivers/core/irq.c`

### Current Code (WRONG)
```c
void disable_irq(uint32_t irq) {
    irq_desc_t* desc = &irq_descriptors[irq];
    
    if (desc->chip && desc->chip->disable) {
        desc->chip->disable(irq);
    }
    
    desc->depth++;  // UNPROTECTED!
    desc->status &= ~IRQ_STATUS_ENABLED;
    desc->status |= IRQ_STATUS_DISABLED;
}
```

### Fixed Code (CORRECT)
```c
void disable_irq(uint32_t irq) {
    if (irq >= MAX_IRQS) return;
    
    irq_desc_t* desc = &irq_descriptors[irq];
    spinlock_t* lock = (spinlock_t*)desc->lock;
    
    if (lock) spin_lock(lock);  // ADD THIS
    
    if (desc->chip && desc->chip->disable) {
        desc->chip->disable(irq);
    }
    
    desc->depth++;
    desc->status &= ~IRQ_STATUS_ENABLED;
    desc->status |= IRQ_STATUS_DISABLED;
    
    if (lock) spin_unlock(lock);  // ADD THIS
}
```

### Apply to All IRQ Functions
- `disable_irq()`
- `disable_irq_nosync()`
- `enable_irq()`
- Any other function that modifies `desc->status` or `desc->depth`

---

## Fix 5: Device Event Callbacks (RACE-005) - TODO

### Location
`kernel/drivers/core/device.c`

### Add Global Lock
```c
#include "../../include/spinlock.h"

// After event_callbacks declaration
static spinlock_t callback_lock;

int device_init(void) {
    spin_lock_init(&callback_lock);  // ADD THIS
    // ... existing code ...
}
```

### Protect Callback Registration
```c
int device_register_event_callback(device_event_callback_t callback) {
    spin_lock(&callback_lock);
    
    if (num_event_callbacks >= MAX_EVENT_CALLBACKS) {
        spin_unlock(&callback_lock);
        return -1;
    }
    
    event_callbacks[num_event_callbacks++] = callback;
    
    spin_unlock(&callback_lock);
    return 0;
}
```

### Protect Callback Invocation
```c
void device_notify_event(device_t* dev, device_event_t event) {
    spin_lock(&callback_lock);
    
    // Make local copy of callback list
    uint32_t count = num_event_callbacks;
    device_event_callback_t local_callbacks[MAX_EVENT_CALLBACKS];
    for (uint32_t i = 0; i < count; i++) {
        local_callbacks[i] = event_callbacks[i];
    }
    
    spin_unlock(&callback_lock);
    
    // Call callbacks outside lock (prevents deadlock)
    for (uint32_t i = 0; i < count; i++) {
        local_callbacks[i](dev, event);
    }
}
```

---

## Testing Strategy

### Phase 1: Single-Core Regression (MUST PASS)
```bash
make clean
make test
./tests/unit/test_scheduler
./tests/unit/test_process
./tests/integration/integration_suite
```

All existing tests MUST pass. If any fail, the fix broke something.

### Phase 2: Race Condition Tests
```bash
# Compile with ThreadSanitizer (requires compiler support)
make SANITIZE=thread

# Run concurrent stress tests
./tests/stress/scheduler_race_test
./tests/stress/process_table_race_test
./tests/stress/irq_race_test
```

### Phase 3: Multi-Core Stress (SMP Required)
```bash
# Boot with SMP enabled
make run-smp CPUS=4

# Run parallel workload
./benchmarks/stress/context_switch_stress --cpus=4 --duration=300
```

---

## Lock Ordering Rules

To prevent deadlocks, ALWAYS acquire locks in this order:

```
Level 1 (Highest):  scheduler_lock
Level 2:            process_table_lock  
Level 3:            callback_lock
Level 4:            heap_lock
Level 5:            global_pmm_lock
Level 6:            per_cpu_cache_lock
Level 7 (Lowest):   irq_desc->lock
```

**NEVER** acquire locks in reverse order!

### Example Violation (BAD)
```c
// BAD: Acquires low-level then high-level
spin_lock(&heap_lock);          // Level 4
spin_lock(&scheduler_lock);     // Level 1 - DEADLOCK RISK!
```

### Correct Usage (GOOD)
```c
// GOOD: Acquires high-level then low-level
spin_lock(&scheduler_lock);     // Level 1
spin_lock(&heap_lock);          // Level 4
```

---

## Performance Impact

### Expected Overhead
- **Single-core:** ~1-2% (spinlock overhead negligible)
- **Multi-core (2 CPUs):** ~3-5% (lock contention minimal)
- **Multi-core (8 CPUs):** ~15-20% (scheduler_lock contention)

### Mitigation Strategies

#### Short-term (After This Fix)
- Minimize critical sections
- Use atomic operations where possible
- Disable interrupts in IRQ handlers to prevent deadlock

#### Long-term (Future Work)
1. **Per-CPU Run Queues** - Eliminates scheduler_lock (Week 1)
2. **RCU for Process Table** - Lock-free reads (Week 2)
3. **Per-CPU Heap Caches** - Like PMM (Week 1)
4. **Lock-free Data Structures** - Advanced (Month 1)

---

## Validation Checklist

Before merging these fixes:

- [ ] All single-core tests pass
- [ ] No new compiler warnings
- [ ] No performance regression > 5% on single-core
- [ ] ThreadSanitizer reports no races
- [ ] Stress tests run for 24 hours without crash
- [ ] Boot works on both single-core and multi-core
- [ ] All locks documented in lock ordering table
- [ ] Code review by 2+ developers
- [ ] Update CHANGELOG.md with fix details

---

## Rollback Plan

If issues occur after merging:

```bash
# Revert scheduler lock
git revert <commit-hash> -m "Revert RACE-001 fix due to deadlock"

# Temporary workaround: Force single-core mode
# Add to kernel command line:
maxcpus=1
```

---

## References

- Audit Report: `docs/RACE_CONDITION_AUDIT.md`
- Patch File: `patches/race_condition_fixes.patch`
- Lock Protocol: Section 8 of audit report
- Testing Guide: `docs/TEST_PLAN.md`
- Bug Tracker: RACE-001 through RACE-005

---

## Next Steps

1. ✅ Review this document with team
2. ✅ Apply Fix 1 (Scheduler) - DONE
3. ⏳ Apply Fix 2 (Process Table) - In Progress
4. ⏳ Apply Fix 3 (PID Allocation)
5. ⏳ Apply Fix 4 (IRQ Status)
6. ⏳ Apply Fix 5 (Device Callbacks)
7. ⏳ Run test suite
8. ⏳ Code review
9. ⏳ Merge to main

**Estimated Time:** 4 hours for all fixes + 2 hours testing = 6 hours total
