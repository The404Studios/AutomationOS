# Execution Model — the unified Job primitive (design, not yet implemented)

> Status: **DESIGN / TARGET.** This documents where the execution architecture is
> heading so that decisions made today (job queue, scheduler, SMP) bend toward one
> primitive instead of four ad-hoc ones. It is deliberately **not** implemented yet
> — see "Migration path" for the gated order in which pieces actually land. The
> guiding rule is the project's: **extract, don't invent.** Every field below is
> introduced only when a concrete job type needs it.

## Why one primitive

Right now we have one real job type (a tensor matmul through `userspace/lib/jobs`)
and several imagined ones (render, physics, asset-streaming). The trap is to grow a
bespoke queue + scheduler for each. The thesis of this document is the owner's:

> Tensor job · Render job · Physics job · Asset-streaming job
> should all be the **same scheduler primitive** — differing only in the function
> they run and the parameters on a shared descriptor.

If they share a descriptor, then one scheduler, one set of run queues, one
work-stealing balancer, and one dependency/fence mechanism serve all of them. That
is the multiplier: improvements to the coordinator improve *every* workload at once.

## Where we are today (the honest baseline)

The current unit of work is intentionally minimal (`userspace/lib/jobs/jobs.h`):

```c
typedef struct { job_fn_t fn; void* arg; } job_t;   // run fn(arg) on a worker
```

Workers pull from a bounded MPMC ring; the submitter drains via a monotonic
`completed` counter; every wait is a blocking futex (no spin — the default kernel is
cooperative). Underneath, the kernel already provides most of the *mechanisms* the
rich model will need — they are just not yet exposed on the job descriptor:

| Capability the model needs | Mechanism that ALREADY exists | Where |
|---|---|---|
| relative importance | priority classes (REALTIME…IDLE) + O(1) MLFQ, real CPU-share | `kernel/core/sched`, `userspace/lib/sched_class.h` |
| "wait until ready" | unified `wait_object` (sleep/futex/join all block on it) | `kernel/core/sched/waitqueue.c` |
| shared-memory workers | real threads (share address space, own stack/regs/FPU) | `userspace/libc/thread.h` |
| hand-off / completion | futex counters + `wait_object` signalling | `userspace/lib/jobs` |

What is missing is (a) a richer descriptor, and (b) **multiple cpus** to make any of
it a throughput win. SMP is tracked separately; this doc is the descriptor.

## The unified Job descriptor (target)

```c
typedef struct job {
    job_fn_t      fn;          // the work; same as today
    void*         arg;

    uint8_t       priority;    // 1. scheduling class / weight  (REALTIME..IDLE)
    uint64_t      deadline_ms; // 2. soft deadline (0 = none)   -> EDF-ish ordering
    int16_t       affinity;    // 3. preferred cpu/worker (-1 = any) -> locality
    job_handle_t  deps[N];     // 4. must complete before this runs -> a fence/wait_object
    uint16_t      n_deps;
    uint32_t      est_cost;    // 5. relative work estimate     -> load balancing / stealing
} job_t;
```

The five fields, each tied to a mechanism we already have or are building — nothing
abstract:

1. **priority** — maps directly onto the existing scheduler priority classes. A
   REALTIME render job preempts an IDLE asset-prefetch job. No new scheduler concept;
   the job inherits the class its worker runs under, or raises it. *Available today.*

2. **deadline** — a soft "should finish by" time (frame deadline for render, audio
   buffer deadline for synth). Lets the dispatcher order ready jobs by urgency
   (earliest-deadline-first) instead of pure priority. *Needs the dispatcher to sort
   ready jobs; the `get_ticks_ms` clock already exists.*

3. **affinity** — the preferred cpu (post-SMP) or worker. This is the field that
   realises the CPU0=scheduler / CPU1=tensor / CPU2=render / CPU3=AI vision: a tensor
   job has `affinity = TENSOR_CPU`. `-1` = run anywhere (lets the balancer steal it).
   *Inert until SMP exists; harmless to carry now.*

4. **dependencies** — a job may name predecessor jobs that must complete first
   (matmul → activation → next layer; asset-decode → upload → draw). This is just a
   **fence built on the `wait_object` we already have**: a job with unmet deps is
   parked on its predecessors' completion signal and is not enqueued as *ready* until
   they fire. This is exactly the unification the wait-object work was for. *The
   primitive exists; the job system needs to consult it.*

5. **execution cost** — a relative estimate (e.g. matmul ≈ M·N·K) used by the load
   balancer / work-stealer to keep cpus evenly loaded and to avoid migrating a job
   whose cost is smaller than the migration overhead. *Inert until per-cpu run queues
   + stealing exist.*

## How each workload becomes the same primitive

| Job type | fn | priority | deadline | affinity | deps | cost |
|---|---|---|---|---|---|---|
| Tensor (matmul tile) | `tensor_matmul` slice | NORMAL/HIGH | none | TENSOR_CPU | input tiles | M·N·K |
| Render (draw cmd) | rasterize cmd | REALTIME | frame deadline | RENDER_CPU | asset uploads | pixels |
| Physics (step) | integrate bodies | HIGH | frame deadline | any | broadphase | bodies |
| Asset streaming | decode/upload | IDLE/BACKGROUND | none | any | none | bytes |

Same struct, same queue, same scheduler. A render frame becomes a small **DAG of
jobs** (decode → upload → draw, draw → composite) expressed entirely through `deps`,
dispatched across cpus by `affinity`, balanced by `est_cost`, ordered by `deadline`.

## Connection to SMP (why this doc and the SMP foundation are siblings)

The descriptor is mostly inert on one cpu — priority and deps work today, but
affinity, cost, and deadline-vs-throughput only *matter* when multiple cpus run
workers simultaneously. So the value curve is:

```
one cpu     -> descriptor proves CORRECT (priority + deps), no throughput change
N cpus      -> affinity + cost + deadline become a real throughput/latency win
```

This is the same honest shape as the job queue itself: correctness first on one core,
the multiplier arrives with SMP. The two efforts share a backbone — `wait_object`
(deps/fences), priority classes (priority), per-cpu run queues (affinity), and the
work-stealer (cost).

## Migration path (gated, each step independently useful — do NOT build it all at once)

1. **priority on jobs** — let a submit carry a class; the worker runs the job at that
   class. Useful immediately (a background prefetch shouldn't starve a tensor job).
2. **dependencies (fences)** — a `job_handle` + "submit when these complete," built on
   `wait_object`. Unlocks job DAGs (the render pipeline, multi-layer tensor) even on
   one core. This is the highest-leverage step and is **single-core-correct**.
3. **affinity** — once SMP lands, route jobs to a preferred cpu/worker; `-1` stays
   stealable. (Inert/no-op until then — safe to introduce the field early.)
4. **execution cost + work stealing** — per-cpu run queues with cost-aware stealing so
   no cpu idles while another has a backlog.
5. **deadline ordering** — EDF among ready jobs for the latency-sensitive types
   (render/audio). Last, because it only pays off once 1–4 create real contention.

## Non-goals / deferred (explicitly)

- Not implementing the full descriptor now. Today's `{fn,arg}` stays until step 1/2
  are genuinely needed by a second job type.
- No general DAG scheduler engine, no priority inheritance, no gang scheduling yet —
  those are real concurrency projects, gated behind SMP being stable.
- This does not change the default build. Like preemption, each step ships gated and
  must keep the default smoke green.

## Bottom line

One descriptor, five fields, each wired to a mechanism we already have or are about to
build for SMP. Carry the cheap inert fields (affinity, cost, deadline) early so the
data model is stable; implement behavior (priority, then dependencies) only as a
concrete second job type demands it. The render/physics/AI workloads then cost almost
nothing to add — they are the same primitive with different parameters.
