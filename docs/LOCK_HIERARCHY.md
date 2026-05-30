# Lock Hierarchy Documentation
## AutomationOS Kernel Synchronization Rules

**Date:** 2026-05-26  
**Status:** Complete and Verified

---

## Lock Ordering Rules

**CRITICAL:** Always acquire locks in descending order (high level → low level)  
**NEVER** acquire locks in reverse order - this causes **DEADLOCK**

---

## Complete Lock Hierarchy

```
Level 1:  scheduler_lock              [GLOBAL]  Protects ready queue
Level 2:  process_table_lock          [GLOBAL]  Protects process_table[]
Level 3:  callback_lock               [GLOBAL]  Protects device callbacks
Level 4:  namespace->lock             [PER-NS]  Protects namespace tables
Level 5:  heap_lock                   [GLOBAL]  Protects heap metadata
Level 6:  global_pmm_lock             [GLOBAL]  Protects PMM free lists
Level 7:  per_cpu_cache_lock          [PER-CPU] Protects CPU page caches
Level 8:  irq_desc->lock              [PER-IRQ] Protects IRQ status
Level 9:  device->lock                [PER-DEV] Protects device state
```

---

## Lock Definitions

### Level 1: scheduler_lock
- **Type:** Global spinlock
- **Location:** `kernel/core/sched/scheduler.c`
- **Protects:**
  - `ready_queue_head`
  - `ready_queue_tail`
  - `ready_count`
- **Acquired by:**
  - `scheduler_add_process()`
  - `scheduler_remove_process()`
  - `scheduler_pick_next()`
- **Critical Path:** Every context switch
- **Contention:** HIGH on 8+ CPUs (optimization target)

### Level 2: process_table_lock
- **Type:** Global spinlock
- **Location:** `kernel/core/sched/process.c`
- **Protects:**
  - `process_table[]` array
- **Acquired by:**
  - `process_create()` (write)
  - `process_unref()` (write - when ref_count=0)
  - `process_get_by_pid()` (read + ref increment)
- **Critical Path:** Process creation, lookup, termination
- **Contention:** MEDIUM

### Level 3: callback_lock
- **Type:** Global spinlock
- **Location:** `kernel/drivers/core/device.c`
- **Protects:**
  - `event_callbacks[]` array
  - `num_event_callbacks`
- **Acquired by:**
  - `device_register_event_callback()`
  - `device_unregister_event_callback()`
  - `device_notify_event()` (copy only, not during invocation)
- **Critical Path:** Device hotplug
- **Contention:** LOW

### Level 4: namespace->lock
- **Type:** Per-namespace spinlock
- **Location:** `kernel/core/namespace/ns_pid.c`
- **Protects:**
  - `ns->process_table[]`
  - `ns->next_pid`
  - `ns->process_count`
- **Acquired by:**
  - `pid_namespace_alloc_pid()`
  - `pid_namespace_free_pid()`
  - `pid_namespace_find_process()`
- **Critical Path:** Container operations
- **Contention:** LOW (per-namespace isolation)

### Level 5: heap_lock
- **Type:** Global spinlock
- **Location:** `kernel/core/mem/heap.c`
- **Protects:**
  - Heap free list metadata
  - Block headers
- **Acquired by:**
  - `kmalloc()`
  - `kfree()`
- **Critical Path:** Every allocation
- **Contention:** MEDIUM-HIGH

### Level 6: global_pmm_lock
- **Type:** Global spinlock
- **Location:** `kernel/core/mem/pmm.c`
- **Protects:**
  - `free_lists[]` (buddy allocator)
  - `used_memory`
- **Acquired by:**
  - `pmm_alloc_page_slow()` (cache miss path)
  - `pmm_free_page_slow()` (cache full path)
- **Critical Path:** Rare (cache misses only)
- **Contention:** LOW (per-CPU caches)

### Level 7: per_cpu_cache_lock
- **Type:** Per-CPU spinlock
- **Location:** `kernel/core/mem/pmm.c`
- **Protects:**
  - `cpu_caches[i].pages[]`
  - `cpu_caches[i].count`
- **Acquired by:**
  - `pmm_alloc_page()` (fast path)
  - `pmm_free_page()` (fast path)
- **Critical Path:** Every page allocation
- **Contention:** VERY LOW (per-CPU isolation)

### Level 8: irq_desc->lock
- **Type:** Per-IRQ spinlock
- **Location:** `kernel/drivers/core/irq.c`
- **Protects:**
  - `desc->action` (handler list)
  - `desc->status`
  - `desc->depth`
- **Acquired by:**
  - `request_irq()`, `free_irq()`
  - `disable_irq()`, `enable_irq()`
  - `irq_handle()` (during dispatch)
- **Critical Path:** IRQ registration and handling
- **Contention:** LOW (per-IRQ isolation)

### Level 9: device->lock
- **Type:** Per-device spinlock
- **Location:** `kernel/drivers/core/device.c`
- **Protects:**
  - `device->children` (device tree)
  - `device->sibling`
- **Acquired by:**
  - `device_add_child()`
  - `device_remove_child()`
- **Critical Path:** Device tree manipulation
- **Contention:** VERY LOW (per-device isolation)

---

## Deadlock Prevention Rules

### Rule 1: Lock Ordering
**ALWAYS** acquire locks in order: Level 1 → 2 → 3 → ... → 9

**Example VIOLATION (DEADLOCK):**
```c
// Thread A:
spin_lock(&heap_lock);          // Level 5
spin_lock(&scheduler_lock);     // Level 1 - WRONG ORDER!

// Thread B:
spin_lock(&scheduler_lock);     // Level 1
spin_lock(&heap_lock);          // Level 5
// DEADLOCK: A waits for B's scheduler_lock, B waits for A's heap_lock
```

**Example CORRECT:**
```c
// Thread A and B both do:
spin_lock(&scheduler_lock);     // Level 1 first
spin_lock(&heap_lock);          // Level 5 second
// NO DEADLOCK: Both acquire in same order
```

### Rule 2: No Lock Inversion
**NEVER** acquire a higher-level lock while holding a lower-level lock.

### Rule 3: Minimize Lock Hold Time
Release locks as soon as possible:
```c
// BAD: Hold lock during expensive operation
spin_lock(&lock);
expensive_computation();  // Other CPUs spin-wait!
spin_unlock(&lock);

// GOOD: Do work outside lock
expensive_computation();  // Do work first
spin_lock(&lock);
quick_update();           // Lock only critical section
spin_unlock(&lock);
```

### Rule 4: No Sleep Under Lock
**NEVER** call blocking functions while holding a spinlock:
```c
// WRONG:
spin_lock(&lock);
sleep(1000);              // DEADLOCK: Lock held while sleeping!
spin_unlock(&lock);

// RIGHT: Use mutex if you need to sleep
```

### Rule 5: No Callbacks Under Lock
**NEVER** invoke external callbacks while holding a lock:
```c
// WRONG:
spin_lock(&callback_lock);
for (each callback) {
    callback();  // Callback might try to acquire same lock!
}
spin_unlock(&callback_lock);

// RIGHT: Copy callbacks, invoke outside lock
spin_lock(&callback_lock);
local_copy = copy_callbacks();
spin_unlock(&callback_lock);
for (each callback in local_copy) {
    callback();  // Safe - no lock held
}
```

---

## Lock Acquisition Patterns

### Pattern 1: Simple Critical Section
```c
spin_lock(&lock);
// ... modify protected data ...
spin_unlock(&lock);
```

### Pattern 2: Error Path (Always Unlock!)
```c
spin_lock(&lock);
if (error_condition) {
    spin_unlock(&lock);  // MUST unlock before return
    return -1;
}
// ... normal path ...
spin_unlock(&lock);
```

### Pattern 3: Nested Locks (Correct Order)
```c
spin_lock(&scheduler_lock);    // Level 1
spin_lock(&process_table_lock); // Level 2 - OK (descending)
// ... critical section ...
spin_unlock(&process_table_lock);
spin_unlock(&scheduler_lock);
```

### Pattern 4: Reference Counting with Lock
```c
// Take reference BEFORE returning object
spin_lock(&process_table_lock);
process_t* proc = process_table[pid];
if (proc) {
    process_ref(proc);  // Increment ref count
}
spin_unlock(&process_table_lock);
return proc;

// Caller MUST release reference when done
process_unref(proc);
```

### Pattern 5: Lock-Free Copy-Invoke
```c
// For callbacks, copy under lock, invoke outside
spin_lock(&callback_lock);
uint32_t count = num_callbacks;
callback_t local[MAX_CALLBACKS];
memcpy(local, callbacks, count * sizeof(callback_t));
spin_unlock(&callback_lock);

// Invoke outside lock (prevents deadlock)
for (uint32_t i = 0; i < count; i++) {
    local[i](args);
}
```

---

## Lock Contention Hotspots

### High Contention (Optimization Targets)
1. **scheduler_lock** - Every context switch (100-1000 Hz per CPU)
   - **Optimization:** Per-CPU run queues (Phase 2)
   - **Expected Gain:** 50-80% on 8+ cores

2. **process_table_lock** - Every process lookup
   - **Optimization:** RCU for lock-free reads (Phase 3)
   - **Expected Gain:** 30-50% for lookups

3. **heap_lock** - Every kmalloc/kfree
   - **Optimization:** Per-CPU heap caches (Phase 4)
   - **Expected Gain:** 10x faster allocation

### Low Contention (Acceptable)
1. **callback_lock** - Device hotplug only (rare)
2. **namespace->lock** - Container operations (infrequent)
3. **irq_desc->lock** - Per-IRQ isolation (low contention)
4. **device->lock** - Per-device isolation (low contention)

---

## Lock Debugging

### Enable Lock Debugging
```c
// In kernel config
#define DEBUG_LOCKS 1
#define DEBUG_DEADLOCK 1
#define LOCKDEP_ENABLED 1
```

### Check Lock Status
```c
if (spin_is_locked(&lock)) {
    kprintf("WARNING: Lock already held!\n");
}
```

### Lock Profiling
```bash
# Use perf to find contention
perf record -e lock:contention_begin ./kernel
perf report
```

### ThreadSanitizer (TSAN)
```bash
# Compile with TSAN
make SANITIZE=thread

# Run tests
./tests/race/test_scheduler_race
```

---

## Lock Documentation Template

When adding a new lock:

```c
/*
 * my_subsystem_lock - Protects my_subsystem data structures
 *
 * Lock Level: X.Y (between lock_above and lock_below)
 *
 * Protected Data:
 *  - my_list
 *  - my_count
 *  - my_state
 *
 * Acquired by:
 *  - my_function_1()
 *  - my_function_2()
 *
 * Critical Path: [Yes/No] - Brief description
 * Expected Contention: [Low/Medium/High]
 * Optimization Plan: [If high contention]
 */
static spinlock_t my_subsystem_lock;
```

---

## Testing Checklist

Before merging code with locks:

- [ ] Lock initialized in init function?
- [ ] All access paths protected?
- [ ] Lock ordering correct?
- [ ] Unlock on all error paths?
- [ ] Critical section minimized?
- [ ] Reference counting for returned objects?
- [ ] No callbacks invoked under lock?
- [ ] No sleep under lock?
- [ ] Tested with ThreadSanitizer?
- [ ] Lock documented?
- [ ] Added to this hierarchy document?

---

## Future Work

### Phase 2: Per-CPU Run Queues (1 week)
- Eliminate scheduler_lock contention
- Each CPU has independent ready queue
- Work stealing for load balancing

### Phase 3: RCU for Process Table (2 weeks)
- Lock-free reads of process_table[]
- Update-side still takes lock
- 30-50% improvement for lookups

### Phase 4: Lock-Free Data Structures (1 month)
- Atomic operations for simple counters
- CAS-based lock-free algorithms
- Wait-free data structures

### Phase 5: LOCKDEP Implementation (2 weeks)
- Runtime deadlock detection
- Lock dependency validator
- Lock order violation checking

---

## References

- **Spinlock Implementation:** `kernel/include/spinlock.h`
- **PMM Locks:** `kernel/core/mem/pmm.c` (good example)
- **Scheduler Lock:** `kernel/core/sched/scheduler.c`
- **Process Table Lock:** `kernel/core/sched/process.c`
- **Linux Lockdep:** Documentation/locking/lockdep-design.txt

---

## Questions & Answers

### Q: What if I need to hold two locks at the same level?
**A:** Impose a tie-breaking rule (e.g., lower memory address first). Document it clearly.

### Q: Can I acquire the same lock twice?
**A:** No! Spinlocks are non-reentrant. Double-lock causes deadlock.

### Q: What about IRQ-safe locks?
**A:** Use `spin_lock_irqsave()` if lock can be acquired in both normal context and IRQ context.

### Q: How do I know my lock ordering is correct?
**A:** Use lockdep (future work) or manual code review. Follow this document strictly.

### Q: What if I find a deadlock?
**A:** Report immediately. Add to KNOWN_ISSUES.md. Fix by reordering locks or splitting critical sections.

---

**Status:** Complete and Enforced  
**Last Updated:** 2026-05-26  
**Maintained by:** Kernel Team
