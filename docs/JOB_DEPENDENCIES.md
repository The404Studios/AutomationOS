# Job dependency graph — DAG jobs on the wait_object (design)

> Status: **DESIGN.** Of the rich-Job fields (priority / deadline / affinity / cost / **dependencies**), dependencies are the **only one with value before SMP exists** — they are expressible *today* on the existing userspace job queue (`userspace/lib/jobs`) plus the `wait_object` fences already in the kernel. This document designs them. It is the highest-leverage non-SMP step in the execution-model roadmap (see `docs/EXECUTION_MODEL.md`).

## The model

A job may name **predecessor jobs that must complete before it becomes runnable.** The queue holds such a job in a *blocked* state until its dependencies fire, then promotes it to *ready*. This turns today's flat job pile into a **DAG**:

```
load_texture ─┐
              ├─> build_mesh ─> render
   (atlas)  ──┘
```
or, for a tensor net:
```
matmul_A ─┐
matmul_B ─┴─> attention ─> softmax
```

## Built on what already exists (extract, don't invent)

Every submitted job already has an implicit completion edge — the monotonic `completed` counter the drain waits on. Promote that to an **explicit completion fence per job: a `wait_object`.** A dependent job blocks on its predecessors' fences *exactly* like sleep / thread-join / futex block on a `wait_object` today. There is **no new primitive** — the fence *is* the wait_object, and it already has the no-lost-wakeup, blocking-not-spinning behavior proven in the job queue. The worker that finishes a job signals that job's fence.

## API sketch

```c
job_handle_t h1 = jobsys_submit(load_texture, &tex);   // submit now returns a handle
job_handle_t h2 = jobsys_submit(matmul_b, &b);
job_handle_t deps[] = { h1, h2 };
jobsys_submit_after(build_mesh, &mesh, deps, 2);        // ready only after h1 AND h2
jobsys_drain();
```

- `job_handle_t` — a small record carrying a **completion fence** (`wait_object`) and a **remaining-deps counter**.
- `jobsys_submit_after(fn, arg, deps, n)` — the job is **parked**, not enqueued ready, until every dep has fired. Each dep completion decrements the dependents' counters; at zero, the job is enqueued ready. A worker finishing a job signals its fence, which fires its dependents' edges.
- `jobsys_submit(fn, arg)` stays as-is (n_deps = 0) — fully backward compatible.

## Single-core correct (works today)

On one CPU this is simply **ordered execution**: `build_mesh` cannot run until `load_texture`'s worker finished and signaled. No SMP required, no races introduced — the wait_object provides the blocking, and a parked job consumes no CPU. So this lands and is useful *now*, on the existing job queue, before any AP exists.

## Why it's the right pre-SMP step

It unlocks real pipelines on a single core — a render frame (decode → upload → draw → composite), a multi-layer net (layer N waits on layer N-1's tiles), an audio graph. And it is **forward-compatible with SMP for free**: when multiple CPUs exist, the *same* DAG parallelizes automatically — independent branches (e.g. `matmul_A` ∥ `matmul_B`) run on different CPUs because they have no edge between them. `affinity` and `cost` later only *tune* placement; the dependency structure is already the thing that exposes the parallelism.

## Migration

1. Today: `jobsys_submit(fn, arg)` — no dependencies.
2. Add: `jobsys_submit` returns a `job_handle_t`; add `jobsys_submit_after(fn, arg, deps, n)`.
3. `jobsys_drain()` is unchanged in meaning (all submitted work complete); with deps it additionally respects ordering automatically.

## Honest scope / deferred

- Design now; **implement when a real DAG workload exists** (the render pipeline or a multi-layer tensor net) — don't build the machinery speculatively.
- It's a **DAG, not a general graph**: cycle-freedom is the submitter's contract (a cycle would deadlock — every job waiting on another). No runtime cycle detection in v1.
- `priority` / `deadline` / `affinity` / `cost` remain deferred — they have no single-core value and ride on SMP + the scheduler, not on this fence mechanism.
