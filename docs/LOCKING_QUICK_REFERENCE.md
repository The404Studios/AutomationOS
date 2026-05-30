# Locking Quick Reference
## AutomationOS Kernel Synchronization Guide

---

## When to Use Locks

### ✅ ALWAYS Lock When:
- Modifying shared global data structures
- Accessing process_table[], ready_queue, or device lists
- Multiple CPUs can access the same data
- Interrupt handlers can access the same data as normal code

### ❌ NO Lock Needed When:
- Accessing per-CPU data (already isolated)
- Accessing process-local data (only one thread can access)
- Using atomic operations (`__atomic_*` functions)
- Read-only access to immutable data

---

## Lock Types

### Spinlock (kernel space)
```c
#include "spinlock.h"

spinlock_t my_lock;

void init_function(void) {
    spin_lock_init(&my_lock);
}

void critical_section(void) {
    spin_lock(&my_lock);
    // ... modify shared data ...
    spin_unlock(&my_lock);
}
```

**When to use:** Short critical sections in kernel code  
**Never:** Use in userspace, or hold while sleeping

### Try-Lock (non-blocking)
```c
if (spin_trylock(&my_lock)) {
    // Got lock
    // ... critical section ...
    spin_unlock(&my_lock);
} else {
    // Lock held by someone else, try later
}
```

---

## Lock Ordering (CRITICAL!)

**ALWAYS acquire locks in this order:**

```
1. scheduler_lock           ← Highest
2. process_table_lock
3. callback_lock
4. heap_lock
5. global_pmm_lock
6. per_cpu_cache_lock
7. irq_desc->lock          ← Lowest
```

### Example: WRONG (Deadlock Risk!)
```c
spin_lock(&heap_lock);          // Level 4
spin_lock(&scheduler_lock);     // Level 1 - WRONG ORDER!
```

### Example: CORRECT
```c
spin_lock(&scheduler_lock);     // Level 1 first
spin_lock(&heap_lock);          // Level 4 second - OK
```

---

## Common Patterns

### 1. Protecting a Global List
```c
static list_t* my_list;
static spinlock_t list_lock;

void add_to_list(item_t* item) {
    spin_lock(&list_lock);
    list_append(my_list, item);
    spin_unlock(&list_lock);
}

item_t* remove_from_list(void) {
    spin_lock(&list_lock);
    item_t* item = list_pop(my_list);
    spin_unlock(&list_lock);
    return item;
}
```

### 2. Protecting a Counter
```c
// Option A: Use atomic (preferred)
static volatile uint64_t counter = 0;

void increment(void) {
    __atomic_add_fetch(&counter, 1, __ATOMIC_SEQ_CST);
}

// Option B: Use lock (if other data needs protection too)
static uint64_t counter = 0;
static spinlock_t counter_lock;

void increment(void) {
    spin_lock(&counter_lock);
    counter++;
    spin_unlock(&counter_lock);
}
```

### 3. Protecting Object with Reference Count
```c
typedef struct {
    spinlock_t lock;
    volatile uint32_t ref_count;
    // ... other fields ...
} object_t;

void object_ref(object_t* obj) {
    __atomic_add_fetch(&obj->ref_count, 1, __ATOMIC_SEQ_CST);
}

void object_unref(object_t* obj) {
    uint32_t old = __atomic_sub_fetch(&obj->ref_count, 1, __ATOMIC_SEQ_CST);
    if (old == 0) {
        // Only one CPU will see transition to 0
        free(obj);
    }
}

void object_modify(object_t* obj) {
    spin_lock(&obj->lock);
    // ... modify obj fields ...
    spin_unlock(&obj->lock);
}
```

### 4. Protecting Interrupt Handler Access
```c
static data_t shared_data;
static spinlock_t data_lock;

void normal_function(void) {
    spin_lock(&data_lock);
    // ... access shared_data ...
    spin_unlock(&data_lock);
}

void interrupt_handler(void) {
    // Same lock works in interrupt context
    spin_lock(&data_lock);
    // ... access shared_data ...
    spin_unlock(&data_lock);
}
```

---

## Atomic Operations

### Increment/Decrement
```c
uint32_t value = 0;

// Returns NEW value
uint32_t new_val = __atomic_add_fetch(&value, 1, __ATOMIC_SEQ_CST);

// Returns OLD value
uint32_t old_val = __atomic_fetch_add(&value, 1, __ATOMIC_SEQ_CST);
```

### Compare-and-Swap
```c
uint32_t expected = 5;
uint32_t new_value = 10;

// Atomically: if (value == expected) { value = new_value; return true; }
bool success = __atomic_compare_exchange_n(&value, &expected, new_value,
                                           false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
```

### Load/Store
```c
// Atomic load
uint32_t val = __atomic_load_n(&value, __ATOMIC_SEQ_CST);

// Atomic store
__atomic_store_n(&value, 42, __ATOMIC_SEQ_CST);
```

---

## Memory Ordering

### When to Use Each:

- **`__ATOMIC_SEQ_CST`** - Default, safest, sequential consistency
- **`__ATOMIC_ACQUIRE`** - For locks (acquire semantics)
- **`__ATOMIC_RELEASE`** - For unlocks (release semantics)
- **`__ATOMIC_RELAXED`** - No ordering (rarely used)

### Example:
```c
// Lock: use ACQUIRE
while (__atomic_test_and_set(&lock, __ATOMIC_ACQUIRE)) {
    // busy wait
}

// Unlock: use RELEASE
__atomic_clear(&lock, __ATOMIC_RELEASE);
```

---

## Common Mistakes

### ❌ MISTAKE 1: Forgetting to Unlock
```c
void bad_function(void) {
    spin_lock(&lock);
    if (error) {
        return;  // BUG: Lock never released!
    }
    spin_unlock(&lock);
}
```

**FIX:**
```c
void good_function(void) {
    spin_lock(&lock);
    if (error) {
        spin_unlock(&lock);
        return;
    }
    spin_unlock(&lock);
}
```

### ❌ MISTAKE 2: Locking in Wrong Order
```c
void bad_function(void) {
    spin_lock(&heap_lock);
    spin_lock(&scheduler_lock);  // WRONG ORDER!
}
```

### ❌ MISTAKE 3: Holding Lock Too Long
```c
void bad_function(void) {
    spin_lock(&lock);
    expensive_computation();  // BAD: Other CPUs spin-wait
    spin_unlock(&lock);
}
```

**FIX:**
```c
void good_function(void) {
    expensive_computation();  // Do work OUTSIDE lock
    
    spin_lock(&lock);
    quick_update();           // Only critical section locked
    spin_unlock(&lock);
}
```

### ❌ MISTAKE 4: Not Using Atomic for Ref Count
```c
void bad_ref(object_t* obj) {
    spin_lock(&obj->lock);
    obj->ref_count++;  // BAD: Non-atomic under lock
    spin_unlock(&obj->lock);
}
```

**FIX:**
```c
void good_ref(object_t* obj) {
    __atomic_add_fetch(&obj->ref_count, 1, __ATOMIC_SEQ_CST);
}
```

---

## Debugging Tips

### Enable Lock Debugging
```c
#define DEBUG_LOCKS 1
#define DEBUG_DEADLOCK 1
```

### Check Lock Status
```c
if (spin_is_locked(&lock)) {
    kprintf("Warning: Lock already held!\n");
}
```

### Lock Profiling
```bash
# Use perf to find lock contention
perf record -e lock:* ./kernel
perf report
```

### ThreadSanitizer
```bash
# Compile with TSAN
make SANITIZE=thread

# Run tests
./tests/race/test_scheduler_race
```

---

## Performance Tips

### 1. Minimize Critical Sections
```c
// BAD: Large critical section
spin_lock(&lock);
prepare_data();      // Slow
update_list();       // Fast
cleanup_data();      // Slow
spin_unlock(&lock);

// GOOD: Small critical section
prepare_data();      // Do outside lock
spin_lock(&lock);
update_list();       // Only lock this
spin_unlock(&lock);
cleanup_data();      // Do outside lock
```

### 2. Use Per-CPU Data
```c
// BAD: Global counter (lock contention)
static uint64_t global_counter;
static spinlock_t counter_lock;

// GOOD: Per-CPU counters (no contention)
DEFINE_PER_CPU(uint64_t, counter);

void increment(void) {
    uint32_t cpu = cpu_id();
    per_cpu(counter, cpu)++;
}
```

### 3. Read-Copy-Update (RCU) for Reads
```c
// For data that's read often, written rarely
// Use RCU instead of locks (future work)
```

---

## Quick Checklist

Before committing code with locks:

- [ ] Lock initialized in init function?
- [ ] All access paths protected?
- [ ] Lock ordering correct?
- [ ] Unlock on all error paths?
- [ ] Critical section minimized?
- [ ] Reference counting for returned objects?
- [ ] Tested with ThreadSanitizer?
- [ ] Documented lock purpose?

---

## Example: Adding a New Lock

```c
// 1. Declare lock (in .c file)
static spinlock_t my_subsystem_lock;

// 2. Initialize lock
void my_subsystem_init(void) {
    spin_lock_init(&my_subsystem_lock);
    // ... other init ...
}

// 3. Document lock
/* 
 * my_subsystem_lock - Protects my_subsystem data structures
 * 
 * Lock ordering: Level 3.5 (between callback_lock and heap_lock)
 * 
 * Protected data:
 *  - my_list
 *  - my_count
 *  - my_state
 */

// 4. Use lock
void my_subsystem_operation(void) {
    spin_lock(&my_subsystem_lock);
    // ... modify protected data ...
    spin_unlock(&my_subsystem_lock);
}

// 5. Test
#ifdef DEBUG_LOCKS
    ASSERT(!spin_is_locked(&my_subsystem_lock), "Lock leaked!");
#endif
```

---

## Resources

- **Audit Report:** `docs/RACE_CONDITION_AUDIT.md`
- **Fix Guide:** `docs/RACE_CONDITION_FIXES.md`
- **Spinlock API:** `kernel/include/spinlock.h`
- **Good Examples:** `kernel/core/mem/pmm.c`, `kernel/core/mem/heap.c`
- **Tests:** `tests/race/`

---

**Remember:** When in doubt, use a lock. Correctness first, optimize later!
