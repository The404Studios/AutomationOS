# MPSC Queue Integration Plan
**Replacing single cpu1_job with lock-free MPSC queue**

> **Status**: INTEGRATION PLAN (SMP Brick 9)  
> **Target**: Replace kernel/arch/x86_64/ap_boot.c single cpu1_job slot with mpsc_queue.h bounded MPSC ring  
> **Discipline**: Brick-by-brick, hard-gated, extractive migration with zero behavior change to existing callers

---

## Executive Summary

The single global `cpu1_job` slot in ap_boot.c is a **ticking time bomb**: it is a single-producer design wearing SMP clothing. The moment a SECOND core (or IRQ-context submitter, or concurrent BSP task) calls `cpu1_submit()` while a job is in-flight, it silently **clobbers** the in-flight fn/arg—a lost job and a cross-core data race on the ownership_t args. The MPSC queue (mpsc_queue.h) IS the multi-entry successor: 256 concurrent jobs, lock-free bounded MPSC algorithm with per-cell sequence numbers for ABA mitigation, and the SAME ownership_t discipline extracted verbatim.

This plan governs the **migration** from 1 slot → 256-slot queue while **preserving backward compatibility** for the existing `cpu1_run()` / `cpu1_submit()` / `cpu1_wait()` API and the proven brick-6/8 self-tests.

---

## 1. When to Migrate

### Trigger Condition: **Brick 9 — Async Completion**

Migrate when:
- ✅ Brick 8 is COMPLETE (dual-core matmul `matmul_self_test()` passes; userspace offload `SYS_CPU1_OFFLOAD` is proven).
- ✅ Brick 8's `cpu1_submit()` + `cpu1_wait()` split API is STABLE (no pending ownership_t contract changes).
- ✅ The ownership model (OWNED→TRANSFERRED→OWNED handoff) is frozen and stress-tested (orphan cleanup proven under process exit).
- ✅ There is a REAL trigger for concurrency:
  - **Scenario A (multi-producer pressure)**: Second BSP task attempts CPU1 offload while a job is in-flight (silent clobber detected via ownership_t assertion or lost result).
  - **Scenario B (async I/O / IRQ submit)**: A future brick (e.g., async I/O completion handlers) wants to submit jobs from IRQ context → cannot block on a single slot.
  - **Scenario C (stress evidence)**: Stress test deliberately hammers `cpu1_submit()` from multiple threads/contexts and observes queue-full (-EAGAIN) as honest pushback vs silent data corruption.

**Do NOT migrate before Brick 8 is frozen.** The queue's API contract (fn/arg/owner_pid) is *extracted* from the proven single-slot design; migrating mid-brick risks chasing a moving target.

### Decision Point

**Q**: Has a second producer (concurrent BSP task, IRQ handler, or future AP) attempted `cpu1_submit()` while a job is in-flight, or is there a documented need for >1 concurrent offload?  
**Yes** → Migrate now (Brick 9).  
**No** → Defer; the single slot is honest about its 1-at-a-time capacity and the MPSC queue is future-ready when needed.

---

## 2. Migration Steps

### Phase 1: Preserve the Old API (Zero Behavior Change)

**Goal**: Keep `cpu1_run()` / `cpu1_submit()` / `cpu1_wait()` / `cpu1_orphan_jobs()` as the **public API** with identical signatures and semantics. Underneath, replace the single `cpu1_job` global with an MPSC queue. Callers (kernel.c, handlers.c) see **zero edits**.

#### Step 1.1: Implement mpsc_queue.c (Brick 9 Implementation)

**File**: `kernel/core/smp/mpsc_queue.c` (new)  
**Scope**: Implement the four API functions declared in mpsc_queue.h:

```c
void mpsc_init(mpsc_queue_t *q);
int  mpsc_enqueue(mpsc_queue_t *q, cpu_job_fn fn, void *arg, uint32_t owner_pid);
int  mpsc_dequeue(mpsc_queue_t *q, cpu_job_fn *out_fn, void **out_arg, uint32_t *out_pid);
void mpsc_orphan_pid(mpsc_queue_t *q, uint32_t exiting_pid);
```

**Algorithm**: Dmitry Vyukov bounded MPMC ring, specialized to MPSC (mpsc_queue.h lines 22-75):
- **enqueue**: CAS on `enq_pos`, acquire cell via seq-diff, write job, RELEASE cell.seq.
- **dequeue**: Single-consumer (CPU1 only), CAS on `deq_pos`, acquire cell via seq-diff, read job, RELEASE cell.seq with next-lap seed.
- **Sequence numbers**: Per-cell ABA-proof version tag (NOT raw head/tail index compare).

**Ownership Integration**: Each cell carries its OWN `ownership_t arg_own` (mpsc_queue.h lines 138). Transition discipline:
- `mpsc_enqueue`: caller's arg is OWNED → transition slot's `arg_own` to TRANSFERRED(to=1).
- `mpsc_dequeue`: slot's `arg_own` is TRANSFERRED(in) → transition to OWNED@CPU1.
- `mpsc_orphan_pid`: scan ring, TRANSFERRED → ORPHANED for matching PIDs.

**Compile Gate**: Guard with `#ifdef SMP_FOUNDATION` (same gate as ap_boot.c).

**Deliverable**: mpsc_queue.c + mpsc_queue.h frozen; compiles; **no callers yet** (ap_boot.c still uses single slot).

#### Step 1.2: Add MPSC Queue Instance to ap_boot.c

**File**: `kernel/arch/x86_64/ap_boot.c`  
**Change**:

```c
#include "../../include/mpsc_queue.h"   // NEW: MPSC queue header

/* OLD (single slot, will be REMOVED in Step 1.4):
static struct {
    cpu_job_fn       fn;
    void            *arg;
    volatile int     pending;
    volatile int     done;
    int              owner_cpu;
    process_t       *owner_proc;
    ownership_t      arg_A;
    ownership_t      arg_B;
    ownership_t      result;
    uint64_t         job_seq;
    uint32_t         owner_pid;
} cpu1_job;
*/

/* NEW: the 256-slot MPSC queue (replaces the single slot) */
static mpsc_queue_t cpu1_queue;
```

Call `mpsc_init(&cpu1_queue);` from `cpu1_job_init()` (ap_boot.c line 840):

```c
void cpu1_job_init(void)
{
    /* OLD: own_init(&cpu1_job.arg_A); etc. */
    mpsc_init(&cpu1_queue);   // NEW: seed all 256 cells
}
```

**Deliverable**: `cpu1_queue` instance exists; `mpsc_init()` is called at boot (before APs start). Still compiles; single-slot API (`cpu1_submit()` etc.) is **unchanged** (next step wraps them).

#### Step 1.3: Wrap Old API Over MPSC Queue

**File**: `kernel/arch/x86_64/ap_boot.c`  
**Scope**: Rewrite `cpu1_submit()` / `cpu1_wait()` / `cpu1_run()` / `cpu1_orphan_jobs()` to delegate to the MPSC queue while **preserving their exact signatures and return semantics**.

##### cpu1_submit() — Enqueue with Overflow Handling

```c
int cpu1_submit(cpu_job_fn fn, void *arg)
{
    if (!fn) {
        return 0;   // NULL fn → nothing submitted (same as before)
    }

    uint32_t owner_pid = current_process ? current_process->pid : 0;
    int ret = mpsc_enqueue(&cpu1_queue, fn, arg, owner_pid);
    
    if (ret == 0) {
        return 1;   // success → OLD API returned 1 for "submitted"
    } else if (ret == -EAGAIN) {
        /* Queue is full (256 jobs in-flight). The OLD single-slot design
         * would have silently clobbered the in-flight job (a BUG). The NEW
         * queue returns honest pushback: the caller MUST handle -EAGAIN.
         * For now, LOG + return 0 (failure) so the caller's bounded-wait
         * sees "job did not run" and can retry or fall back. */
        kprintf("[SMP] cpu1_submit: queue FULL (256 jobs) → -EAGAIN\n");
        return 0;   // treat as failure (caller will see cpu1_wait timeout)
    }
    return 0;       // any other error (shouldn't happen)
}
```

**Critical Difference**: The old single-slot `cpu1_submit()` ALWAYS succeeded (by clobbering). The new queue-based submit can FAIL (-EAGAIN when full). The wrapper **downgrades -EAGAIN to 0** (failure) to match the old API's "submitted=1 or 0" boolean return, logging the overflow. Callers already handle `cpu1_submit() == 0` → `cpu1_wait()` will timeout → graceful fallback. **No caller edits needed.**

##### cpu1_wait() — Poll MPSC Dequeue Until Job Drains

The OLD `cpu1_wait()` spun on the single slot's `done` flag. The NEW version must **poll the queue** until the submitted job is consumed by CPU1's worker loop (ap_main).

**Problem**: MPSC dequeue happens on **CPU1** (the consumer), not the BSP. The BSP (producer/waiter) has no direct "done" flag per-job anymore — it must infer completion by **waiting for the queue to drain past its submitted position**.

**Solution (deferred result cell per job — Phase 2 refinement)**: For Phase 1 (preserving the old API), we can implement a **synchronous wait shim**:

1. **Hack (Phase 1 only)**: Keep a SINGLE result flag `volatile int cpu1_last_job_done` that CPU1's worker loop sets after running ANY job. The BSP waits on this flag (ACQUIRE-poll with TSC deadline). This works ONLY if there is ONE producer (the BSP) — exactly the assumption the old API made. It is NOT multi-producer safe, but it preserves backward compat for the brick-6/8 self-tests.

2. **Future (Phase 2)**: Each `mpsc_cell_t` gains a `volatile int done` flag (or the producer allocates a stack-local result struct and passes a pointer in `arg`). The BSP waits on **its specific job's done flag** rather than a shared global. This enables true multi-producer concurrency.

For **Phase 1** (minimal migration):

```c
static volatile int cpu1_last_job_done = 0;   // HACK: single-waiter shim

int cpu1_wait(uint64_t deadline_tsc)
{
    /* OLD API: ACQUIRE-poll cpu1_job.done until set or deadline.
     * NEW API (Phase 1 shim): ACQUIRE-poll the shared "last job done" flag.
     * This ONLY works if there is ONE waiter (the BSP). Multi-producer
     * migration (Phase 2) replaces this with per-job result cells. */
    while (__atomic_load_n(&cpu1_last_job_done, __ATOMIC_ACQUIRE) == 0) {
        if (rdtsc() >= deadline_tsc) {
            return 0;   // timeout → job did not finish
        }
        __asm__ volatile("pause" ::: "memory");
    }
    
    /* Reset the flag for the next job (single-waiter assumption). */
    __atomic_store_n(&cpu1_last_job_done, 0, __ATOMIC_RELAXED);
    return 1;   // job completed
}
```

**Critical Limitation**: This **does NOT support concurrent waiters**. If two BSP tasks submit jobs concurrently and both wait, the second waiter may see the first's done flag and return prematurely. This is **acceptable for Phase 1** because the existing call sites (kernel.c matmul self-test, handlers.c offload) are **serialized** (one-at-a-time). Multi-waiter support is Phase 2.

##### cpu1_run() — Unchanged (Submit + Wait)

```c
int cpu1_run(cpu_job_fn fn, void *arg)
{
    if (!cpu1_submit(fn, arg)) {
        return 0;   // submit failed (queue full or NULL fn)
    }
    uint64_t deadline = rdtsc() + AP_WAIT_US * AP_TSC_PER_US;  // 100ms
    return cpu1_wait(deadline);
}
```

**No edits needed** — `cpu1_run()` already delegates to submit + wait; the new implementations handle the queue underneath.

##### cpu1_orphan_jobs() — Delegate to MPSC Orphan Hook

```c
void cpu1_orphan_jobs(uint32_t exiting_pid)
{
    mpsc_orphan_pid(&cpu1_queue, exiting_pid);
}
```

**Verbatim delegation** — the MPSC queue's `mpsc_orphan_pid()` scans all 256 cells and transitions matching PIDs' `arg_own` from TRANSFERRED → ORPHANED, exactly as the single-slot version did.

#### Step 1.4: Update CPU1 Worker Loop (ap_main)

**File**: `kernel/arch/x86_64/ap_boot.c`, `ap_main()` function (line 256)  
**OLD** (single-slot poll):

```c
for (;;) {
    if (__atomic_load_n(&cpu1_job.pending, __ATOMIC_ACQUIRE)) {
        cpu1_job.fn(cpu1_job.arg);
        __atomic_store_n(&cpu1_job.done, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&cpu1_job.pending, 0, __ATOMIC_RELEASE);
    } else {
        ap1_idle_ticks++;
        __asm__ volatile("pause");
    }
}
```

**NEW** (MPSC dequeue poll):

```c
for (;;) {
    cpu_job_fn fn = NULL;
    void *arg = NULL;
    uint32_t pid = 0;
    
    if (mpsc_dequeue(&cpu1_queue, &fn, &arg, &pid)) {
        /* Job dequeued. The cell's arg_own is now OWNED by CPU1 (mpsc_dequeue
         * did the TRANSFERRED→OWNED transition). Run the job. */
        fn(arg);
        
        /* Signal the BSP waiter (Phase 1 shim). This is the RELEASE-store
         * that makes the job's results visible before cpu1_wait returns. */
        __atomic_store_n(&cpu1_last_job_done, 1, __ATOMIC_RELEASE);
    } else {
        /* Queue empty. Idle poll (same as the single-slot's "no pending job"). */
        ap1_idle_ticks++;
        __asm__ volatile("pause");
    }
}
```

**Ordering**: `mpsc_dequeue()` ACQUIRE-loads the cell's seq (sees the producer's RELEASE-store), so CPU1 reads the fresh fn/arg. The `cpu1_last_job_done` RELEASE-store publishes fn's side effects to the BSP's ACQUIRE-poll in `cpu1_wait()`.

#### Step 1.5: Remove Single-Slot Global

**File**: `kernel/arch/x86_64/ap_boot.c`  
**Delete**: The `cpu1_job` struct (ap_boot.c line 164–176) — no longer referenced.  
**Sanity**: Grep for `cpu1_job.` → should find ZERO hits after this step (only `cpu1_queue` remains).

**Deliverable**: ap_boot.c compiles; old API (`cpu1_run()`, `cpu1_submit()`, etc.) is **preserved**, now backed by the MPSC queue. Zero edits to callers (kernel.c, handlers.c).

---

### Phase 2: Multi-Producer Stress Test & Per-Job Results (Future Brick)

**Scope** (NOT in the initial migration; this is the **follow-on** brick after Brick 9):

1. **Per-job result cells**: Each `mpsc_cell_t` gains a `volatile int done` flag (or the caller passes a stack-local result struct pointer in `arg`). The waiter spins on **its specific job's done flag** instead of the shared `cpu1_last_job_done` shim. This enables **concurrent waiters** (multiple BSP tasks or IRQ handlers each waiting on their own submitted job).

2. **Stress test (1000 concurrent submissions)**: See Section 4 below.

3. **Deprecate the shim**: Remove `cpu1_last_job_done` once per-job results are proven.

---

## 3. Backward Compatibility

### Preserved API Contracts

| Function | Signature | Behavior (Before) | Behavior (After) |
|----------|-----------|-------------------|------------------|
| `cpu1_submit(fn, arg)` | `int` → 1=success, 0=failure | Always 1 if `fn != NULL` (silent clobber if slot busy) | 1 if enqueued, 0 if queue full (-EAGAIN logged) |
| `cpu1_wait(deadline)` | `int` → 1=done, 0=timeout | ACQUIRE-poll single slot's `done` flag | ACQUIRE-poll shared `cpu1_last_job_done` (Phase 1 shim) |
| `cpu1_run(fn, arg)` | `int` → 1=done, 0=timeout/fail | submit + wait with 100ms deadline | **Unchanged** (delegates to new submit/wait) |
| `cpu1_orphan_jobs(pid)` | `void` | TRANSFERRED→ORPHANED on single slot | Scan 256 cells; TRANSFERRED→ORPHANED for matching PID |

### Caller Impact

**Zero edits required** to:
- `kernel/kernel.c`: `matmul_self_test()` calls `cpu1_submit()` + `cpu1_wait()` → works as-is.
- `kernel/core/syscall/handlers.c`: `sys_cpu1_offload()` calls `cpu1_submit()` + `cpu1_wait()` → works as-is.
- Any future caller using the `cpu1_run()` / `cpu1_submit()` / `cpu1_wait()` API → no changes.

**New Failure Mode**: `cpu1_submit()` can now return 0 (queue full). Existing callers already check the return value:

```c
if (!cpu1_submit(fn, arg)) {
    return 0;   // already handled as "submit failed"
}
```

They will see the timeout in `cpu1_wait()` and log/return gracefully (same as if CPU1 wedged). **No silent corruption** (the win).

### Ownership Contract

**Preserved verbatim**:
- Producer: arg buffer is OWNED → `cpu1_submit()` transitions to TRANSFERRED(to=1) → producer must NOT touch arg until wait completes.
- Consumer (CPU1): `mpsc_dequeue()` transitions TRANSFERRED(in) → OWNED@CPU1 → CPU1 may read arg.
- Completion: (Phase 1 shim doesn't reclaim; Phase 2 per-job results will transition back to OWNED@producer when done.)
- Orphan: exiting process → `cpu1_orphan_jobs()` → TRANSFERRED → ORPHANED (buffers drain without UAF).

**No new states, no new transitions** — the MPSC queue's `ownership_t` per-cell is the SAME discipline, just 256 copies instead of 1.

---

## 4. Testing Strategy

### Stress Test: 1000 Concurrent Submissions

**Goal**: Prove the MPSC queue handles **multi-producer contention** without lost jobs, data races, or silent clobbers.

**Scope**: Deliberately **exceed the 256-entry capacity** to exercise the -EAGAIN overflow path and validate that the queue does NOT drop jobs.

#### Test Harness

**File**: `tests/test_mpsc_stress.c` (new)

```c
#include "../kernel/include/mpsc_queue.h"
#include "../kernel/include/kernel.h"
#include "../kernel/include/perf.h"

#define STRESS_JOBS 1000    // exceeds 256 queue capacity
static volatile long stress_counters[STRESS_JOBS] = {0};

/* Trivial job: increment its counter slot (proves it ran). */
static void stress_job(void *arg)
{
    long idx = (long)arg;
    stress_counters[idx]++;
}

void test_mpsc_stress(void)
{
    /* Submit 1000 jobs as fast as possible. The first 256 will enqueue; the
     * rest should get -EAGAIN. Retry -EAGAIN jobs in a second pass. */
    int submitted = 0, retries = 0;
    for (long i = 0; i < STRESS_JOBS; i++) {
        int ret = mpsc_enqueue(&cpu1_queue, stress_job, (void*)i, 0);
        if (ret == 0) {
            submitted++;
        } else if (ret == -EAGAIN) {
            /* Queue full. The job is NOT dropped — we'll retry it below. */
            retries++;
        }
    }

    kprintf("[STRESS] First pass: %d submitted, %d retries needed\n",
            submitted, retries);

    /* Give CPU1 time to drain some jobs (bounded spin, NOT infinite). */
    uint64_t drain_deadline = rdtsc() + 1000000ULL * 3000ULL;  // ~1s
    while (rdtsc() < drain_deadline) {
        __asm__ volatile("pause");
    }

    /* Retry the jobs that got -EAGAIN. */
    for (long i = 0; i < STRESS_JOBS; i++) {
        if (stress_counters[i] == 0) {  // job hasn't run yet
            int ret = mpsc_enqueue(&cpu1_queue, stress_job, (void*)i, 0);
            if (ret == 0) submitted++;
        }
    }

    /* Wait for all jobs to drain (bounded deadline, NOT infinite). */
    drain_deadline = rdtsc() + 5000000ULL * 3000ULL;  // ~5s
    while (rdtsc() < drain_deadline) {
        int all_done = 1;
        for (long i = 0; i < STRESS_JOBS; i++) {
            if (stress_counters[i] == 0) {
                all_done = 0;
                break;
            }
        }
        if (all_done) break;
        __asm__ volatile("pause");
    }

    /* Verify: every job ran exactly ONCE. */
    int success = 1;
    for (long i = 0; i < STRESS_JOBS; i++) {
        if (stress_counters[i] != 1) {
            kprintf("[STRESS] FAIL: job %ld ran %ld times (expected 1)\n",
                    i, stress_counters[i]);
            success = 0;
        }
    }

    if (success) {
        kprintf("[STRESS] PASS: all %d jobs ran exactly once\n", STRESS_JOBS);
    } else {
        kprintf("[STRESS] FAIL: some jobs lost or duplicated\n");
    }
}
```

#### Test Execution

**When**: After Phase 1 migration is complete (ap_boot.c uses MPSC queue; old API preserved).

**Trigger**: Add `test_mpsc_stress();` to kernel.c after `matmul_self_test()` (gated by a new `#ifdef SMP_STRESS_TEST`).

**Pass Criteria**:
- ✅ All 1000 jobs run exactly ONCE (no lost jobs, no duplicates).
- ✅ No ownership_t assertions fire (no TRANSFERRED→TRANSFERRED re-transition, no access-while-TRANSFERRED).
- ✅ No silent clobbers (counters are all 1, not 0 or >1).
- ✅ -EAGAIN is logged when the queue fills (honest pushback, not a crash).

#### Multi-Producer Variant (Phase 2)

**Scope**: Spawn 4 kernel threads (or 4 APs, if Brick 10+ lands multi-AP support) that each submit 250 jobs concurrently → 1000 total jobs from **4 producers** racing on the MPSC queue.

**Pass Criteria**: Same as above (all jobs run once, no races). This proves the lock-free CAS retry in `mpsc_enqueue()` handles contention without losing jobs.

---

## 5. Rollback Plan

### Failure Scenarios

| Scenario | Detection | Rollback |
|----------|-----------|----------|
| **MPSC queue loses jobs** | Stress test: `stress_counters[i] == 0` after drain deadline | Revert to single slot; add `cpu1_queue` to a `#if 0` block |
| **Ownership assertion fires** | Boot panic: "own_transition: invalid state" during enqueue/dequeue | Revert ownership transitions in mpsc_queue.c; verify single-slot transitions still work |
| **Queue "full" (-EAGAIN) when it shouldn't be** | `mpsc_depth()` reports <256 but enqueue returns -EAGAIN (seq-diff bug) | Fix sequence-number seed in `mpsc_init()` (verify cell[i].seq == i at init) |
| **CPU1 worker loop spins forever** | `ap1_idle_ticks` stops climbing; no jobs drain | Revert `ap_main()` to single-slot pending/done poll; verify dequeue logic |
| **Backward-compat break** | Existing brick-6/8 self-tests FAIL after migration | Revert old API wrapper (cpu1_submit/wait); verify shim logic matches single-slot ordering |

### Rollback Procedure

1. **Revert ap_boot.c**:
   - Restore the single `cpu1_job` struct (from git history: `git show HEAD~1:kernel/arch/x86_64/ap_boot.c > ap_boot.c`).
   - Remove `#include "mpsc_queue.h"`.
   - Remove `mpsc_init(&cpu1_queue);` from `cpu1_job_init()`.

2. **Revert ap_main()**:
   - Restore the single-slot `pending/done` poll (git diff `HEAD~1` ap_main).

3. **Keep mpsc_queue.c/h**:
   - Move to `#if 0` block or a `kernel/core/smp/mpsc_queue_deferred.c` file so it stays in tree but is not compiled.
   - Add a comment: "DEFERRED: Brick 9 MPSC queue — rollback due to [issue]. Single-slot `cpu1_job` is active."

4. **Re-test**:
   - Verify brick-6/8 self-tests PASS with the single slot restored.
   - Log rollback in `docs/SMP_ROLLBACK_LOG.md` with the failure symptom + root cause.

### Rollback Decision Point

**Trigger rollback if**:
- Stress test shows >1% job loss (any `stress_counters[i] != 1` after retries).
- Ownership assertion fires during normal boot (not just stress).
- `matmul_self_test()` or `sys_cpu1_offload()` FAILS after migration (regression).

**Do NOT rollback if**:
- -EAGAIN is logged under extreme stress (>256 concurrent jobs) — that is **correct behavior** (honest pushback). Fix: increase queue size to 512 or add caller-side retry logic.
- `cpu1_last_job_done` shim has a race under multi-waiter load — that is a **Phase 1 limitation**. Fix: implement Phase 2 per-job result cells instead of rolling back.

---

## 6. Success Criteria

**Phase 1 (Backward-Compatible Migration) is COMPLETE when**:

1. ✅ `mpsc_queue.c` is implemented and compiles (gated by `SMP_FOUNDATION`).
2. ✅ `ap_boot.c` uses `mpsc_queue_t cpu1_queue` instead of the single `cpu1_job` slot.
3. ✅ Old API (`cpu1_run()`, `cpu1_submit()`, `cpu1_wait()`, `cpu1_orphan_jobs()`) is **preserved** with identical signatures.
4. ✅ Brick-6 self-test (`worktest` sum) PASSES (CPU1 runs BSP-supplied job, result is correct).
5. ✅ Brick-8 self-test (`matmul_self_test`) PASSES (dual-core speedup, bit-identical result).
6. ✅ Userspace offload (`SYS_CPU1_OFFLOAD`) PASSES (ring-3 app offloads matmul to CPU1, correct result + `by_apic==1`).
7. ✅ Stress test (1000 jobs) PASSES (all jobs run exactly once, no lost jobs, -EAGAIN logged when full).
8. ✅ No ownership_t assertions during stress (OWNED→TRANSFERRED→OWNED transitions are clean).
9. ✅ Zero edits to callers (kernel.c, handlers.c) — backward compatibility proven.

**Phase 2 (Multi-Producer Concurrency) is COMPLETE when** (deferred to future brick):

1. ✅ Per-job result cells replace the `cpu1_last_job_done` shim.
2. ✅ Multi-producer stress test (4 threads × 250 jobs = 1000 concurrent) PASSES.
3. ✅ Concurrent waiters (2+ BSP tasks waiting on different jobs) do NOT see each other's done flags.

---

## 7. Integration Checklist

**Pre-Migration**:
- [ ] Brick 8 is FROZEN (matmul + offload self-tests pass; no pending ownership_t changes).
- [ ] Ownership model stress-tested (process exit orphan cleanup proven under load).
- [ ] MPSC queue algorithm reviewed (seq-number ABA mitigation understood; CAS retry bounds verified).

**Phase 1 Implementation**:
- [ ] `mpsc_queue.c` implemented (`mpsc_init`, `mpsc_enqueue`, `mpsc_dequeue`, `mpsc_orphan_pid`).
- [ ] `mpsc_queue.c` compiles under `SMP_FOUNDATION` gate.
- [ ] `cpu1_queue` instance added to `ap_boot.c`.
- [ ] `mpsc_init(&cpu1_queue)` called from `cpu1_job_init()`.
- [ ] Old API (`cpu1_submit` / `cpu1_wait` / `cpu1_run` / `cpu1_orphan_jobs`) rewritten as MPSC wrappers.
- [ ] `ap_main()` worker loop rewritten to poll `mpsc_dequeue()` instead of `cpu1_job.pending`.
- [ ] `cpu1_last_job_done` shim added (Phase 1 single-waiter compatibility).
- [ ] Single-slot `cpu1_job` struct removed (grep for `cpu1_job.` → zero hits).

**Testing**:
- [ ] Brick-6 self-test (`worktest`) PASSES.
- [ ] Brick-8 self-test (`matmul_self_test`) PASSES (speedup + correctness).
- [ ] Userspace offload (`SYS_CPU1_OFFLOAD`) PASSES (ring-3 matmul offload works).
- [ ] Stress test (1000 jobs) implemented (`tests/test_mpsc_stress.c`).
- [ ] Stress test PASSES (all jobs run once, no lost jobs).
- [ ] No ownership_t assertions during stress.
- [ ] -EAGAIN logged when queue fills (>256 jobs in-flight).

**Documentation**:
- [ ] This plan (`MPSC_INTEGRATION_PLAN.md`) reviewed and approved.
- [ ] Brick-9 completion report written (`docs/SMP_BRICK9_COMPLETION.md`) documenting:
  - MPSC algorithm + per-cell ownership model.
  - Backward-compat shim (`cpu1_last_job_done`) and its Phase 1 limitation.
  - Stress test results (jobs submitted, -EAGAIN count, drain time).
  - Rollback procedure (if triggered).

**Phase 2 (Future)**:
- [ ] Per-job result cells designed (replace `cpu1_last_job_done`).
- [ ] Multi-producer stress test (4 threads × 250 jobs) implemented.
- [ ] Concurrent waiters verified (no cross-waiter done-flag pollution).
- [ ] Phase 1 shim removed.

---

## 8. File Inventory

**New Files** (Brick 9):
- `kernel/core/smp/mpsc_queue.c` — MPSC queue implementation (enqueue, dequeue, orphan).
- `tests/test_mpsc_stress.c` — 1000-job stress test.

**Modified Files**:
- `kernel/arch/x86_64/ap_boot.c` — Replace `cpu1_job` with `cpu1_queue`; rewrite old API as wrappers; update `ap_main()`.
- `kernel/include/mpsc_queue.h` — (Already frozen; no edits unless ABI changes.)

**Unchanged Files** (zero edits, backward compat verified):
- `kernel/kernel.c` — `matmul_self_test()` calls old API → works as-is.
- `kernel/core/syscall/handlers.c` — `sys_cpu1_offload()` calls old API → works as-is.

**Documentation**:
- `docs/MPSC_INTEGRATION_PLAN.md` — This file.
- `docs/SMP_BRICK9_COMPLETION.md` — Post-migration completion report (written after Phase 1 lands).

---

## 9. Rationale

### Why NOW (Brick 9)?

The single `cpu1_job` slot is a **concurrency hazard** disguised as working code:
- It works TODAY because there is exactly ONE producer (the BSP, serialized).
- It will **silently corrupt data** the moment a second producer (concurrent BSP task, IRQ handler, or second AP) calls `cpu1_submit()` while a job is in-flight.
- The MPSC queue is the **extracted, proven successor** — 256-entry bounded ring with the SAME ownership_t discipline, just lock-free multi-producer instead of single-producer.

Migrating NOW (after Brick 8 is frozen) is **cheap** because the contract is stable. Migrating LATER (after more bricks depend on the single slot) is expensive because we'd be chasing a moving API.

### Why Preserve the Old API?

**Extract, don't invent**: The brick-6/8 self-tests and the userspace offload are **proven**. They validate the ownership model, the TSC-bounded wait, and the cross-core ordering. Changing their call sites introduces **new risk** (did we preserve the ordering? did we break the ownership transition?). Keeping the old API as a **compatibility shim** over the new queue means:
- Zero caller edits → zero regression risk on proven paths.
- The shim is **load-bearing evidence** that the MPSC queue is a drop-in replacement (if the shim breaks, the queue's contract is wrong).
- Future bricks can use the **new queue API directly** (`mpsc_enqueue` / `mpsc_dequeue`) while old bricks keep using the old API — both work.

### Why Phase 1 (Single-Waiter Shim) vs Phase 2 (Per-Job Results)?

**Phase 1** is the **minimal migration** that preserves backward compat:
- The `cpu1_last_job_done` shim works for the existing call sites (kernel.c, handlers.c) because they are **serialized** (one job at a time).
- It is **simple** (one shared flag, same ACQUIRE/RELEASE ordering as the old `done` flag).
- It **proves the queue works** without requiring per-job result infrastructure.

**Phase 2** (per-job results) is the **real multi-producer unlock**, but it requires:
- Per-cell `done` flags (or result structs) → ABI change to `mpsc_cell_t`.
- Waiter-side logic to track "which job is mine" → stack-local result pointer or job ID.
- Stress test with **concurrent waiters** (not just concurrent submitters).

Doing both at once is **high risk** (two moving parts). Doing Phase 1 first is **incremental** (prove the queue, then prove per-job results separately).

---

## 10. Open Questions

1. **Queue size (256 vs 512 vs 1024)?**  
   **Answer**: 256 is the frozen size (mpsc_queue.h line 95). It fits in 32 KiB (.bss) and is 256× more capacity than the single slot. If stress tests show >10% -EAGAIN under realistic load, bump to 512 (64 KiB) in a follow-on brick. Do NOT change during Phase 1 migration (verify 256 works first).

2. **Should `cpu1_submit()` BLOCK on -EAGAIN instead of failing?**  
   **Answer**: NO (violates async discipline). The caller must NEVER block indefinitely waiting for queue space — that is a **liveness hazard** (if CPU1 wedges, the BSP hangs). The correct fix is:
   - Return -EAGAIN (honest pushback) → caller decides: retry, run inline, or shed load.
   - For the old API wrapper: downgrade -EAGAIN to 0 (failure) so `cpu1_wait()` sees timeout → graceful fallback (log "CPU1 queue full" instead of hanging).

3. **What if CPU1 wedges mid-job (infinite loop in `fn`)?**  
   **Answer**: Same as today (the single slot has this hazard too). The BSP's `cpu1_wait()` has a **bounded TSC deadline** (100ms for liveness, ~2s for compute) — on timeout it logs + returns 0 (failure). The wedged job stays in-flight; its `owner_pid` is orphaned if the process exits. The queue does NOT deadlock (other jobs can still enqueue; they just won't drain until the wedged job is preempted or CPU1 is reset). This is a **future brick** (per-CPU preemptive timer on the AP, or a watchdog that detects stuck jobs).

4. **Can IRQ handlers call `cpu1_submit()`?**  
   **Answer**: YES (lock-free enqueue is IRQ-safe), but the IRQ handler must NOT call `cpu1_wait()` (blocking in IRQ context is forbidden). The IRQ handler should:
   - `cpu1_submit(fn, arg)` → enqueue the job.
   - Return immediately (async job, no wait).
   - The job's result is picked up via a callback or polled later by a kernel thread.
   This is **Phase 2** (async completion); Phase 1's `cpu1_wait()` shim assumes the waiter is a schedulable BSP task, not an IRQ handler.

---

## 11. Summary

**Brick 9 (MPSC Integration) replaces the single `cpu1_job` slot with a 256-entry lock-free MPSC queue while preserving the existing `cpu1_run()` / `cpu1_submit()` / `cpu1_wait()` API for backward compatibility.** The migration is **incremental** (Phase 1 = single-waiter shim, Phase 2 = per-job results), **hard-gated** (stress-tested under 1000-job load), and **rollback-ready** (single-slot fallback if the queue loses jobs). Zero edits to callers (kernel.c, handlers.c); the proven brick-6/8 self-tests pass as-is. The queue unlocks **multi-producer concurrency** (future bricks can submit from IRQ context, multiple APs, or concurrent BSP tasks) while the old API degrades gracefully on overflow (-EAGAIN → retry or fallback).

**When to migrate**: After Brick 8 is frozen and a **real trigger** for concurrency exists (second producer, IRQ-context submit, or stress evidence of silent clobbers).  
**Rollback if**: Stress test shows >1% job loss, ownership assertions fire, or brick-6/8 self-tests regress.  
**Success criteria**: All jobs run exactly once, no silent clobbers, -EAGAIN logged on overflow, zero caller edits.

---

**End of Plan.**
