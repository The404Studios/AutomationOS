# Memory Ownership Primitive (Brick 0)

**Status**: Implemented, compile-verified, gated (not yet live)  
**Branch**: smp-foundation  
**Files**: `kernel/include/kref.h`, `kernel/core/mem/kref.c`, `kernel/include/ownership.h`, `kernel/core/mem/ownership.c`  
**SMP Roadmap**: Brick 0 (foundation) — discipline established, later bricks integrate into subsystems

---

## Overview

The memory ownership primitive provides atomic reference counting (`kref_t`) and explicit ownership state management (`ownership_t`) for kernel memory allocations. This is the foundation for safe memory sharing across multiple owners (file descriptors, shared memory, network buffers, SMP-shared data structures) and prevents use-after-free, double-free, and cross-CPU data races.

### Two-Layer Architecture

1. **kref.h / kref.c** — Liveness tracking (is the object still alive?)
2. **ownership.h / ownership.c** — Access discipline (who can touch it, and how?)

These layers are complementary, not redundant:
- **refcount** answers "how many handles keep it alive"
- **ownership state** answers "what the holder of a handle may legally do"

---

## kref_t API (Liveness)

### Core Types

```c
typedef struct {
    uint32_t count;
} kref_t;
```

### Allocation API

#### `kmalloc_ref(size_t size) → void*`
Allocate `size` bytes of refcounted memory with refcount=1.

**Returns**: Pointer to payload (NOT the header), or NULL on failure.

**Example**:
```c
void *obj = kmalloc_ref(1024);  // 1KB allocation, refcount=1
if (!obj) {
    kprintf("Allocation failed\n");
    return;
}
```

---

#### `kmalloc_ref_dtor(size_t size, void (*dtor)(void*)) → void*`
Allocate `size` bytes with an optional destructor callback.

**Parameters**:
- `size` — Payload size in bytes
- `dtor` — Destructor function (called with payload pointer before free), or NULL

**Returns**: Pointer to payload, or NULL on failure.

**Example**:
```c
void my_cleanup(void *payload) {
    kprintf("Cleaning up %p\n", payload);
    // Release locks, close FDs, etc.
}

void *obj = kmalloc_ref_dtor(512, my_cleanup);
// my_cleanup will be called when refcount reaches 0
```

---

#### `kget(void *ptr) → void*`
Increment reference count atomically (+1 ref).

**Returns**: The same pointer on success, NULL if the pointer is invalid or corrupted (magic canary check failed).

**Example**:
```c
void *shared = kget(obj);  // refcount: 1 → 2
```

---

#### `kput(void *ptr) → int`
Decrement reference count atomically (-1 ref). If this was the last reference, calls the destructor (if any), poisons the magic canary, and frees the allocation.

**Returns**: 1 if freed, 0 otherwise.

**Example**:
```c
kput(obj);      // refcount: 2 → 1
kput(shared);   // refcount: 1 → 0, frees
```

---

### Low-Level kref Operations (Embedded Use)

For objects that embed `kref_t` directly (not via `kmalloc_ref`):

#### `kref_init(kref_t *k)`
Initialize refcount to 1 (object alive with one owner).

#### `kref_get(kref_t *k)`
Increment refcount atomically. Saturates at `KREF_SATURATED` (0xFFFFFFFF) instead of wrapping.

#### `kref_put(kref_t *k) → int`
Decrement refcount atomically. Returns 1 if this was the last reference (caller should destroy the object), 0 otherwise.

#### `kref_read(const kref_t *k) → uint32_t`
Read the current reference count (debug/diagnostics only; never branch on this for correctness — the value may be stale by the time you see it).

---

### Hidden Header Layout

`kmalloc_ref` allocations prepend a hidden header before the payload:

```c
typedef struct kref_hdr {
    kref_t   ref;    // Reference counter
    uint32_t magic;  // KREF_MAGIC (0xCAFEBEEF) when alive, 0 when freed
    uint32_t size;   // Payload size in bytes
    void (*dtor)(void *payload);  // Optional destructor (NULL if none)
} kref_hdr_t;
```

The user pointer returned by `kmalloc_ref` points to the payload immediately after this header. The header is internal; callers only see the payload pointer.

---

### Safety Features

1. **SEQ_CST atomics** — Full sequential consistency, matches `process.c` / `cow.c` house style
2. **Saturating counter** — Prevents overflow UAF; saturated objects are intentionally leaked (safe failure mode)
3. **Magic canary** — Detects use-after-free / double-free / heap corruption (0xCAFEBEEF when alive, 0 when freed)
4. **NULL-safe** — All ops (kget/kput) are no-ops on NULL pointers
5. **Underflow guard** — Double-put detection (logs error, does not free)

---

## ownership_t API (Access Discipline)

### Core Types

```c
typedef enum own_state {
    OWN_OWNED       = 0,  // Exclusive read+write by one owner
    OWN_SHARED      = 1,  // Multiple readers, no writers
    OWN_BORROWED    = 2,  // Temporary lease (nested within owner's lifetime)
    OWN_TRANSFERRED = 3,  // Ownership moved to new owner
    OWN_ORPHANED    = 4,  // Owner gone, object scheduled for cleanup
} own_state_t;

typedef struct ownership {
    kref_t     ref;          // Embedded liveness counter
    uint32_t   magic;        // OWN_MAGIC (0x4F574E45 = "OWNE")
    uint8_t    state;        // own_state_t
    uint8_t    flags;        // OWN_FLAG_* orthogonal attributes
    uint8_t    _pad[2];
    uint32_t   owner_cpu;    // CPU that currently owns (or OWN_CPU_NONE)
    uint32_t   borrow_depth; // Nested borrows outstanding
    spinlock_t lock;         // Serializes state transitions
} ownership_t;
```

### Attribute Flags

```c
#define OWN_FLAG_NONE        0x00u
#define OWN_FLAG_MUTABLE     0x01u  // BORROWED: permits writes
#define OWN_FLAG_DEFER_FREE  0x02u  // ORPHANED: cleanup is queued
#define OWN_FLAG_TRACED      0x04u  // Emit kprintf trace on each transition
#define OWN_FLAG_PINNED      0x08u  // In-flight DMA: may not free even at refcount=0
```

---

### Lifecycle API

#### `own_init(ownership_t *o)`
Initialize ownership descriptor: state=OWNED, refcount=1, owner_cpu=cpu_id(), borrow_depth=0.

**Example**:
```c
ownership_t own;
own_init(&own);  // Birth: OWNED by this CPU
```

---

#### `own_get(ownership_t *o)`
Increment refcount (only legal on SHARED state).

**ILLEGAL on any other state** — calling it on OWNED/BORROWED/TRANSFERRED/ORPHANED is a violation (panics). This catches the classic "I refcounted an exclusively-owned object and now two CPUs think they own it" bug.

**Example**:
```c
own_share(&own);       // OWNED → SHARED
void *copy = own_get(&own);  // Legal: take another reader
```

---

#### `own_put(ownership_t *o) → int`
Decrement refcount. Returns 1 if this drove refcount 1→0 AND the object is reclaimable (borrow_depth==0, not PINNED).

If refcount hits 0 but the object is not reclaimable (borrowed or pinned), transitions to ORPHANED+DEFER_FREE and returns 0. The last `own_return`/`own_unpin` then completes the free.

**Returns**: 1 if freed, 0 otherwise.

**Example**:
```c
if (own_put(&own)) {
    kfree(container_of(&own, my_obj_t, ownership));
}
```

---

### State Transitions

#### `own_transition(ownership_t *o, own_state_t to, uint32_t new_owner_cpu) → int`
The single chokepoint all lifecycle changes flow through. Validates the edge against the transition table and dynamic invariants, updates owner_cpu, and (if OWN_FLAG_TRACED) emits a trace.

**PANICS** on an illegal edge (these are memory-safety invariants, not debug niceties).

**Parameters**:
- `to` — Target state
- `new_owner_cpu` — Used only for →TRANSFERRED/→OWNED (pass OWN_CPU_NONE otherwise)

**Returns**: 0 on success.

**Transition Table**:

```
from \ to     OWNED  SHARED  BORROWED  TRANSFERRED  ORPHANED
OWNED           .      Y        Y           Y           Y
SHARED          Y*     Y        Y           .           Y
BORROWED        Y      .        Y           .           Y**
TRANSFERRED     Y      Y        Y           Y           Y
ORPHANED        .      .        .           .           Y(self)
```

- **Y** = always legal
- **Y*** = SHARED → OWNED only when refcount==1 (last reader re-acquires exclusivity)
- **Y**** = BORROWED → ORPHANED only records intent; lease must END (`own_return`) before reclaim
- **.** = ILLEGAL

---

### Convenience Wrappers

#### `own_share(ownership_t *o)`
OWNED → SHARED. Releases exclusivity; multiple readers may now hold refs.

**Example**:
```c
own_share(&net_buf->ownership);  // Share across CPUs
```

---

#### `own_transfer(ownership_t *o, uint32_t to_cpu)`
→ TRANSFERRED. Ownership MOVES to `to_cpu`. Original owner's handle is poisoned (any access by original owner after this is a violation).

**Example**:
```c
own_transfer(&job->ownership, 1);  // Transfer tensor job CPU0 → CPU1
```

---

#### `own_orphan(ownership_t *o)`
→ ORPHANED. The owner is gone (exited/crashed/revoked) but the object is not yet reclaimed (e.g. in-flight DMA must drain, or a borrower still holds a lease). Terminal state except for the final free.

**Example**:
```c
own_orphan(&task->files[fd].ownership);  // Task exited, file still in use
```

---

### Borrow / Lease API

#### `own_borrow(ownership_t *o, int mutable, int take_ref)`
Take a temporary, non-owning lease. The lender remains the real owner; the borrower may read (and write iff `mutable!=0`) but MUST NOT free, transfer, or re-lend.

**Parameters**:
- `mutable` — If non-zero, sets OWN_FLAG_MUTABLE (permits writes)
- `take_ref` — If non-zero, increments refcount (for async borrows that may outlive the lender's stack frame)

**Example**:
```c
// Stack-scoped borrow (synchronous, no refcount)
own_borrow(&buf->ownership, 1, 0);  // Mutable borrow, no ref
// ... use buf ...
own_return(&buf->ownership);  // Release lease
```

```c
// Async borrow (may outlive lender's stack frame)
own_borrow(&buf->ownership, 0, 1);  // Immutable borrow, +1 ref
// ... hand buf to async callback ...
// Callback calls own_return + own_put when done
```

---

#### `own_return(ownership_t *o)`
End a lease. Decrements borrow_depth. When depth hits 0, restores the prior state (OWNED, or completes a deferred ORPHAN free).

**MUST balance** `own_borrow` (I6: borrow-nesting invariant).

**Example**:
```c
own_return(&buf->ownership);  // Release lease
```

---

### HW Pin / Unpin (DMA)

#### `own_pin(ownership_t *o)`
Set OWN_FLAG_PINNED. While pinned, `own_put` never frees even at refcount=0 (it ORPHAN+DEFERs instead). This is the "a bus stall is not a software spin" discipline: HW may still be reading the buffer after the last software ref drops.

**Example**:
```c
own_pin(&dma_buf->ownership);  // DMA engine is using this
e1000_submit_dma(dma_buf);
```

---

#### `own_unpin(ownership_t *o)`
Clear OWN_FLAG_PINNED. If a free was deferred while pinned, and the object is now reclaimable, completes the free.

**MUST balance** `own_pin`.

**Example**:
```c
// DMA completion interrupt
own_unpin(&dma_buf->ownership);  // DMA done, may now free
if (own_put(&dma_buf->ownership)) {
    kfree(dma_buf);
}
```

---

### Debug / Violation Detection

#### `own_validate(const ownership_t *o)`
Structural validity check (always-on, zero-cost in release). Checks magic canary and state range. PANICS if corrupted.

**Called at the top of every op** — a bad magic or out-of-range state means memory corruption, never safe to ignore.

---

#### `own_assert_can_read(const ownership_t *o, uint32_t cur_cpu)`
DEBUG build assertion: current CPU is allowed to read this object.

**Checks**:
- TRANSFERRED: only new owner may read
- ORPHANED: nobody establishes fresh access
- OWNED: owner only

---

#### `own_assert_can_write(const ownership_t *o, uint32_t cur_cpu)`
DEBUG build assertion: current CPU is allowed to write this object.

**Checks**:
- SHARED: read-only (writers must leave SHARED first)
- BORROWED: writes only if OWN_FLAG_MUTABLE set
- TRANSFERRED: only new owner may write

**Example**:
```c
void modify_buffer(net_buf_t *buf) {
    own_assert_can_write(&buf->ownership, cpu_id());
    buf->data[0] = 0x42;  // Safe: ownership model enforces exclusivity
}
```

---

#### `own_is_reclaimable(const ownership_t *o) → int`
Check if the object can be freed: borrow_depth==0 AND not PINNED. Always-on guard (I3: no-free-while-borrowed / no-free-while-pinned).

---

#### `own_dump(const ownership_t *o)`
One-line dump for panic context / debug commands.

**Output**:
```
ownership 0x1234: state=OWNED owner_cpu=0 refs=1 borrows=0 flags=0x00
```

---

## Ownership Model

### Who Owns What, When

| State        | Owner                        | Refcount         | Read Access       | Write Access       |
|--------------|------------------------------|------------------|-------------------|--------------------|
| OWNED        | One exclusive owner          | 1                | Owner only        | Owner only         |
| SHARED       | Multiple readers             | ≥1               | All holders       | **None** (read-only) |
| BORROWED     | Lender (owner_cpu = lender)  | 1+               | Borrower          | Borrower (if MUTABLE) |
| TRANSFERRED  | New owner (owner_cpu updated)| 1+               | New owner only    | New owner only     |
| ORPHANED     | None (scheduled for cleanup) | 0 (or deferred)  | **None**          | **None**           |

### Key Invariants

**I1. Exactly-one-state**: state is always a valid own_state_t (enforced by ASSERT_ALWAYS).

**I2. SHARED-iff-multi**: refcount > 1 ⇒ state == OWN_SHARED (the ONLY state where count may exceed 1).

**I3. No-free-while-borrowed**: own_put at 1→0 with borrow_depth>0 PANICS (borrower still holds a lease).

**I4. No-access-after-transfer**: original owner_cpu touching a TRANSFERRED object is a violation (caught by own_assert_can_write/read).

**I5. Orphan-is-terminal**: ORPHANED only transitions to itself; it is never re-owned, re-shared, or re-borrowed.

**I6. Borrow-nesting**: own_return must balance own_borrow; borrow_depth==0 is required before OWNED→TRANSFERRED or before final free.

---

## Integration Points

### 1. Embedding in Data Structures

Embed `ownership_t` by value in any object that needs lifetime+access tracking:

```c
typedef struct net_buf {
    ownership_t ownership;  // Lifecycle state
    uint8_t     data[MTU];
    size_t      len;
} net_buf_t;

net_buf_t *alloc_net_buf(void) {
    net_buf_t *buf = kmalloc(sizeof(net_buf_t));
    own_init(&buf->ownership);  // Birth: OWNED by this CPU
    return buf;
}

void free_net_buf(net_buf_t *buf) {
    if (own_put(&buf->ownership)) {  // Last ref?
        kfree(buf);
    }
}
```

---

### 2. kmalloc_ref Integration (Future Brick)

`kmalloc_ref` will be extended to embed `ownership_t` in the header (alongside `kref_t`):

```c
typedef struct kref_hdr {
    ownership_t own;  // Replaces standalone kref_t
    uint32_t    magic;
    uint32_t    size;
    void (*dtor)(void *payload);
} kref_hdr_t;
```

This makes ownership tracking automatic for all refcounted allocations.

---

### 3. Network Buffers (SMP Brick 4)

```c
// CPU0: allocate and send
net_buf_t *buf = alloc_net_buf();
own_transfer(&buf->ownership, 1);  // Transfer to CPU1 (NIC IRQ handler)
e1000_send(buf);

// CPU1 (NIC IRQ): DMA completion
void e1000_irq_handler(void) {
    net_buf_t *buf = completed_tx_buf;
    own_transfer(&buf->ownership, 0);  // Transfer back to CPU0
    free_net_buf(buf);  // CPU0 frees
}
```

---

### 4. Graphics Compositor (SMP Brick 5)

```c
// Borrow a surface for read-only compositing
own_borrow(&surface->ownership, 0, 0);  // Immutable, no ref
compositor_blit(surface);
own_return(&surface->ownership);  // Release lease
```

---

### 5. AI Tensor Jobs (SMP Brick 6)

```c
// Transfer ownership of a tensor job to worker CPU
own_transfer(&job->ownership, worker_cpu);
schedule_on_cpu(worker_cpu, job);

// Worker CPU: execute and return ownership
void tensor_worker(tensor_job_t *job) {
    own_assert_can_write(&job->ownership, cpu_id());
    execute_tensor_op(job);
    own_transfer(&job->ownership, scheduler_cpu);  // Return to scheduler
}
```

---

## Edge Case Handling

### 1. Double-Free

**kref_t**: Magic canary poisoned to 0 on free. Second `kput` logs error, does not free.

**ownership_t**: Magic canary poisoned to 0xDEAD0000 | state on free. `own_validate` PANICS.

---

### 2. Use-After-Free

**kref_t**: Magic canary check in `kget`/`kput` catches UAF (magic != KREF_MAGIC).

**ownership_t**: `own_validate` checks magic == OWN_MAGIC at the top of every op.

---

### 3. Refcount Overflow

**kref_t**: Saturates at KREF_SATURATED (0xFFFFFFFF). Saturated objects are intentionally leaked (safe failure mode vs. UAF).

**ownership_t**: Inherits kref saturation. Saturated objects never free.

---

### 4. Refcount Underflow (Double-Put)

**kref_t**: `kref_put` detects underflow (count wraps to huge value). Logs error, does not free.

**ownership_t**: Inherits kref underflow guard.

---

### 5. Access After Transfer

**Caught by**: `own_assert_can_read`/`own_assert_can_write` (DEBUG builds).

**Example**:
```c
own_transfer(&obj->ownership, 1);  // Transfer to CPU1
obj->data[0] = 42;  // ← VIOLATION: original owner touches after transfer
// own_assert_can_write PANICS: owner_cpu != cur_cpu
```

---

### 6. Free While Borrowed

**Caught by**: `own_put` checks `own_is_reclaimable()` (I3 invariant).

**Behavior**: Transitions to ORPHANED+DEFER_FREE, returns 0. Last `own_return` completes the free.

**Example**:
```c
own_borrow(&buf->ownership, 0, 0);  // Borrow
own_put(&buf->ownership);  // ← refcount 1→0, but borrow_depth>0
// Result: ORPHANED+DEFER_FREE, not freed yet
own_return(&buf->ownership);  // ← Final return completes the free
```

---

### 7. Free While Pinned (DMA)

**Caught by**: `own_put` checks `own_is_reclaimable()` (I3 invariant).

**Behavior**: Transitions to ORPHANED+DEFER_FREE, returns 0. `own_unpin` completes the free.

**Example**:
```c
own_pin(&dma_buf->ownership);  // DMA in flight
own_put(&dma_buf->ownership);  // ← refcount 1→0, but PINNED
// Result: ORPHANED+DEFER_FREE, not freed yet
own_unpin(&dma_buf->ownership);  // ← DMA done, completes the free
```

---

### 8. Nested Borrows

**Supported**: `borrow_depth` tracks nesting. Each `own_borrow` +1, each `own_return` -1.

**Example**:
```c
own_borrow(&obj->ownership, 0, 0);  // depth: 0→1
own_borrow(&obj->ownership, 0, 0);  // depth: 1→2 (nested)
own_return(&obj->ownership);        // depth: 2→1
own_return(&obj->ownership);        // depth: 1→0, restores OWNED
```

---

### 9. Shared → Owned

**Legal only when** refcount==1 (last reader re-acquires exclusivity). Enforced in `own_transition`.

**Example**:
```c
own_share(&obj->ownership);    // OWNED → SHARED, refcount=1
// Other readers call own_get/own_put...
// Last reader:
own_transition(&obj->ownership, OWN_OWNED, cpu_id());  // SHARED → OWNED
// Only succeeds if refcount==1
```

---

### 10. Concurrent State Corruption

**Protected by**: `ownership_t.lock` (spinlock). State and owner_cpu are updated atomically inside the lock.

**Validation**: `own_transition` re-validates state inside the lock (catch concurrent corruption).

---

## Usage Examples

### Example 1: Simple Allocation and Cleanup

```c
void example_basic(void) {
    void *obj = kmalloc_ref(1024);
    if (!obj) return;
    // Use obj...
    kput(obj);  // refcount: 1→0, frees
}
```

---

### Example 2: Sharing Across Multiple Owners

```c
void example_sharing(void) {
    void *obj = kmalloc_ref(256);
    void *shared1 = kget(obj);  // refcount: 1→2
    void *shared2 = kget(obj);  // refcount: 2→3
    kput(obj);      // refcount: 3→2
    kput(shared1);  // refcount: 2→1
    kput(shared2);  // refcount: 1→0, frees
}
```

---

### Example 3: With Destructor

```c
void my_destructor(void *payload) {
    kprintf("Cleaning up %p\n", payload);
}

void example_destructor(void) {
    void *obj = kmalloc_ref_dtor(512, my_destructor);
    kput(obj);  // Calls my_destructor before free
}
```

---

### Example 4: Ownership State Machine

```c
typedef struct task {
    ownership_t ownership;
    // ...
} task_t;

void example_ownership(void) {
    task_t task;
    own_init(&task.ownership);  // Birth: OWNED by CPU0

    // Share across CPUs
    own_share(&task.ownership);  // OWNED → SHARED
    // Other CPUs call own_get/own_put...

    // Transfer ownership to CPU1
    own_transfer(&task.ownership, 1);  // → TRANSFERRED

    // CPU1: re-acquire exclusive ownership
    own_transition(&task.ownership, OWN_OWNED, 1);  // → OWNED by CPU1

    // CPU1: orphan (task exiting, but refs may exist)
    own_orphan(&task.ownership);  // → ORPHANED

    // Last ref drops
    if (own_put(&task.ownership)) {
        kfree(&task);
    }
}
```

---

### Example 5: Borrow Pattern

```c
void example_borrow(net_buf_t *buf) {
    // Stack-scoped immutable borrow (no refcount)
    own_borrow(&buf->ownership, 0, 0);
    compositor_blit(buf);  // Read-only access
    own_return(&buf->ownership);
}

void example_async_borrow(net_buf_t *buf) {
    // Async mutable borrow (takes refcount)
    own_borrow(&buf->ownership, 1, 1);  // Mutable, +1 ref
    schedule_async_write(buf, async_callback);
}

void async_callback(net_buf_t *buf) {
    // Modify buf...
    own_return(&buf->ownership);  // Release lease
    own_put(&buf->ownership);     // Drop ref
}
```

---

### Example 6: DMA Pinning

```c
void example_dma(dma_buf_t *buf) {
    own_pin(&buf->ownership);  // Mark in-flight
    e1000_submit_dma(buf);

    // DMA completion interrupt (runs on arbitrary CPU)
    void dma_irq_handler(void) {
        own_unpin(&buf->ownership);  // DMA done
        if (own_put(&buf->ownership)) {  // Last ref?
            kfree(buf);
        }
    }
}
```

---

## Debugging

### Enable Per-Object Tracing

```c
ownership_t own;
own_init(&own);
own.flags |= OWN_FLAG_TRACED;  // Emit kprintf on each transition
```

**Output**:
```
[OWN] init 0x1234: state=OWNED owner_cpu=0 refs=1
[OWN] transition 0x1234: OWNED -> SHARED (owner_cpu=0 refs=1 borrows=0)
[OWN] get 0x1234: refs=2
[OWN] put 0x1234: refs=1 (still alive)
[OWN] put 0x1234: 0 refs, reclaimable -> FREEING
```

---

### Dump Ownership State

```c
own_dump(&obj->ownership);
// Output: ownership 0x1234: state=OWNED owner_cpu=0 refs=1 borrows=0 flags=0x00
```

---

### Assertions in DEBUG Builds

```c
void modify_buffer(net_buf_t *buf) {
    own_assert_can_write(&buf->ownership, cpu_id());  // PANICS if violation
    buf->data[0] = 0x42;
}
```

---

## Design Principles (Extract, Don't Invent)

The ownership primitive reuses the kernel's existing primitives verbatim:

- **kref_t** for liveness (atomic refcounting)
- **spinlock_t** for transition critical sections
- **cpu_id()** for owner attribution
- **ASSERT / ASSERT_ALWAYS / kernel_panic** for violation detection
- **SEQ_CST atomics** (matches process.c / cow.c style)

Nothing here introduces a new locking or atomics convention. The ownership model is a discipline layered on top of existing mechanisms, not a parallel universe.

---

## SMP Bring-Up Discipline

This file is **Brick 0** of the 8-brick SMP roadmap: the ownership MODEL, implemented and compile-verified, but **NOT yet live** in any allocator or subsystem.

**Gating**: AP-online ≠ ownership-model-live.

**Integration timeline** (later bricks):
- **Brick 1**: Integrate into `kmalloc_ref` (automatic for all refcounted allocations)
- **Brick 2**: Per-CPU run queues (scheduler ownership tracking)
- **Brick 3**: First AP executes code (checkpoint before enabling)
- **Brick 4**: Network buffers (e1000 TX/RX ownership transfer)
- **Brick 5**: Graphics compositor (surface borrow pattern)
- **Brick 6**: AI tensor jobs (cross-CPU job transfer)
- **Brick 7**: Per-CPU stats (ownership of statistics buffers)
- **Brick 8**: Full SMP validation (stress testing under load)

Each brick is checkpoint-verified before the next.

---

## File Placement

```
kernel/include/kref.h          — kref_t API
kernel/core/mem/kref.c         — kref_t implementation
kernel/include/ownership.h     — ownership_t API + state machine
kernel/core/mem/ownership.c    — ownership_t implementation
KREF_USAGE_EXAMPLES.c          — Usage examples
docs/MEMORY_OWNERSHIP.md       — This file
```

`kernel/core/mem/` placement rationale: Sits next to `cow.c` (the existing phys-page refcount table). Same subsystem, same SEQ_CST style. `cow.c` is the precedent for "refcount → free-on-zero" living under `core/mem`.

---

## Related Documentation

- **SMP_ARCHITECTURE.md** — Full SMP roadmap (8-brick plan)
- **LOCKING_QUICK_REFERENCE.md** — Spinlock usage patterns
- **RACE_CONDITION_AUDIT.md** — SMP safety analysis

---

## Quick Reference Card

| Operation               | kref_t API                     | ownership_t API                        |
|-------------------------|--------------------------------|----------------------------------------|
| Allocate                | `kmalloc_ref(size)`            | (embed `ownership_t`, call `own_init`) |
| Initialize              | `kref_init(&k)`                | `own_init(&o)`                         |
| Add reference           | `kget(ptr)` or `kref_get(&k)`  | `own_get(&o)` (SHARED only!)           |
| Drop reference          | `kput(ptr)` or `kref_put(&k)`  | `own_put(&o)`                          |
| Share (multi-reader)    | (automatic via kget)           | `own_share(&o)` → SHARED               |
| Transfer ownership      | (n/a)                          | `own_transfer(&o, to_cpu)`             |
| Borrow (temporary lease)| (n/a)                          | `own_borrow(&o, mut, take_ref)` + `own_return(&o)` |
| Pin for DMA             | (n/a)                          | `own_pin(&o)` + `own_unpin(&o)`        |
| Orphan (owner gone)     | (n/a)                          | `own_orphan(&o)`                       |
| Assert read access      | (n/a)                          | `own_assert_can_read(&o, cpu_id())`    |
| Assert write access     | (n/a)                          | `own_assert_can_write(&o, cpu_id())`   |
| Debug trace             | (kprintf manually)             | Set `OWN_FLAG_TRACED` + `own_dump(&o)` |

---

**End of MEMORY_OWNERSHIP.md**
