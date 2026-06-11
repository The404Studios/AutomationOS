# Per-CPU structures — the `cpu_t` abstraction (design)

> Status: **DESIGN.** Introduce the `cpu_t` struct **early — even while CPU count is 1** — so SMP bricks 4–7 become a clean refactor instead of a rewrite. The single-CPU version is a deliberate functional no-op. This is the same "extract, don't invent / build the concrete case first" move that worked for the scheduler and wait_object.

## Why introduce it at N=1

The current scheduler refers to *the* current task, *the* runqueue, *the* idle task — globals that implicitly assume one CPU. SMP doesn't change the scheduling *logic*; it changes *whose* runqueue/current/idle you mean. If we rename those globals into `cpus[cpu_id()].field` now (with `cpu_id()` still returning 0), the scheduler stops saying "the current task" and starts saying "**this CPU's** current task" — which is the entire conceptual leap for SMP. Doing it at N=1 is a no-op we can verify byte-for-byte against today's behavior; doing it later, after APs exist, is a high-risk rewrite under concurrency.

## The struct (grounded in the current scheduler's globals)

```c
typedef struct cpu {
    uint32_t     id;          // LAPIC id; 0 = BSP
    process_t*   current;     // currently-running task   (was the global "current")
    process_t*   idle;        // this CPU's idle task
    runqueue_t   rq;          // this CPU's O(1) MLFQ (active/expired, 140 levels)
    cpu_stats_t  stats;       // ctx_switches, cpu_ticks  (the stats brick 49 added)
    /* added by later bricks, not now:
       spinlock_t  rq_lock;   // brick 6: per-CPU runqueue lock
       volatile int online;   // brick 3: AP marks itself online
       ipi_mailbox_t ipi;     // brick 7+: cross-CPU wakeups
       tlb_shoot_t  tlb;      // when SMP TLB shootdown lands           */
} cpu_t;

static cpu_t cpus[MAX_CPUS];
static int   ncpu = 1;                 // brick 0's MADT count sets this
#define this_cpu()  (&cpus[cpu_id()])  // cpu_id()==0 today (stubs.c)
```

## What is per-CPU vs what stays global

| Per-CPU (moves into `cpu_t`) | Global / shared (exactly one instance) |
|---|---|
| current task, idle task | the process table (all tasks) |
| the MLFQ runqueue (brick 6) | wait_objects + the sleep list (a task on CPU A may be woken by CPU B) |
| scheduling stats | the kernel heap, the page allocator |
| (later) rq lock, IPI mailbox | address spaces / CR3s, the `as_refcount` cells |

The crucial subtlety for later: **wait_objects are shared, not per-CPU.** `wait_object_signal` from a worker on CPU B must be able to wake a task that will resume on CPU A. That's a brick-7 concern (cross-CPU wakeup needs an IPI to kick an idle CPU), called out here so the per-CPU split doesn't accidentally privatize the wait queues.

## Migration path (each step independently verifiable)

1. **Now (the early refactor, ~brick 4 pulled forward):** replace the scheduler's `current` / `idle` / runqueue / stats globals with `this_cpu()->field`. With `cpu_id()==0` this is a pure rename — **must pass 38/38 and the preempt stress identically.** No behavior change; purely structural.
2. **Brick 6:** give each CPU its own `rq`; `enqueue(task)` chooses a CPU (initially always CPU 0).
3. **Brick 7:** tasks migrate between CPUs' runqueues (load balance); cross-CPU wake via IPI.
4. **Brick 8:** an idle CPU steals from a busy CPU's runqueue (cost-aware, per the execution model).

## The `cpu_id()` hinge

Today `stubs.c::cpu_id()` returns 0, so `this_cpu()` is always `&cpus[0]`. Once an AP is up (brick 3+), `cpu_id()` reads the LAPIC ID instead. **Scheduler code written against `this_cpu()` then resolves correctly with zero edits** — the abstraction absorbs the single→multi transition. (Note: `smp.c` also defines `cpu_id()`, colliding with `stubs.c`'s — when the real `cpu_id()` lands, the stub is deleted, not both kept.)

## Honest scope

The N=1 refactor changes **no behavior** and is gated by nothing — it must be a verified no-op (38/38 + preempt stress identical), or it's wrong. All the *value* is structural: it's the difference between bricks 4–7 being a careful rename versus a concurrent rewrite. The real multi-CPU behavior (separate runqueues, migration, stealing) is bricks 6–8 and stays gated until APs are stable.
