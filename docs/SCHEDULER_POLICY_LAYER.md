# Scheduler Policy Layer — north-star design (resource-adaptive scheduler)

Status: DESIGN (deliverable 2 of the F3-3 design+verify workflow). NOT all code today.
F3-3 ships only the `scheduler_choose_cpu` seam over the existing F3-2 fields; the
types/layers below are the shape that seam + those fields grow into, brick by brick,
**wrapping the existing MLFQ, never replacing it**.

The scheduler is a **compute coordination layer**, not just "pick next process". A task
asks "which CPU should I run on?" (`scheduler_choose_cpu`); a CPU asks "which task should
I run?" (the existing per-CPU MLFQ `scheduler_pick_next`). The adaptive scheduler is built
entirely out of (a) which-CPU branches in `choose_cpu` and (b) which-task fallback branches
in a future `pick_best_task_for_cpu` — two thin policy funnels around an untouched mechanism.

## The model (types land at F3-6, not now)

```c
// WHAT a task wants from a CPU. Its first two members ALREADY EXIST as the loose
// F3-2 process_t fields — the profile is a future home that ADOPTS them (struct-wrap,
// not a rewrite: p->sched.allowed_cpus is literally today's p->allowed_cpus relocated).
typedef struct sched_profile {
    uint64_t      allowed_cpus;     // == today's process_t.allowed_cpus (F3-2, EXISTS)
    uint32_t      pinned_cpu;       // == today's process_t.pinned_cpu  (F3-2, EXISTS)
    sched_class_t sclass;           // role/urgency class (F3-6; seed = NORMAL)
    uint32_t      last_cpu;         // cache-warm hint; seed = queued_cpu (EXISTS)
    int32_t       nice;             // == today's process_t.priority (EXISTS, relocated)
    uint8_t       latency_sensitive;// interactive flag (F3-8); seed 0
} sched_profile_t;

typedef enum sched_class {
    SCHED_CLASS_RECOVERY = 0,   // watchdog/self-heal — outranks normal (law 6)
    SCHED_CLASS_INTERACTIVE,    // latency-protected (law 4): compositor, shell
    SCHED_CLASS_NORMAL,         // default desktop fairness (today's behavior)
    SCHED_CLASS_BATCH,          // fills idle only (law 5): matmul/offload/smpstress
    SCHED_CLASS_PINNED_RT       // hard-pinned specialized worker: cpu1hello today
} sched_class_t;

typedef enum cpu_role {
    CPU_ROLE_GENERAL = 0,       // runs anything legal (cpu0 today)
    CPU_ROLE_INTERACTIVE,       // reserved for latency-sensitive (future cpu0 split)
    CPU_ROLE_BATCH,             // offload/compute host (cpu1's eventual role)
    CPU_ROLE_PINNED_WORKER      // hosts hard-pinned tasks (cpu1 in F3-3)
} cpu_role_t;

// A future VIEW over today's cpu_t (online/ready_count/rq_lock/rq_active/...), not a
// parallel struct — adopting it is renaming reads, not copying.
typedef struct cpu_sched_state {
    int        online;          // EXISTS (cpu_t.online) — legality input
    cpu_role_t role;            // F3-6; seed cpu0=GENERAL, cpu1=PINNED_WORKER
    uint32_t   ready_count;     // EXISTS (cpu_t.ready_count) — load input
    uint32_t   pressure;        // F3-9 counter (runnable-but-waiting); seed 0
} cpu_sched_state_t;
```

## The four layers (strict order)

1. **Hard legality** (correctness; never skipped). online + `allowed_cpus` + `pinned_cpu`.
   Two enforcement points: the policy `choose_cpu` (advisory) AND the enqueue gate in
   `scheduler_add_process_to_cpu` (mandatory, refuses off-mask). So even a buggy future
   balancer can never enqueue off-affinity. *Legality before optimization, made structural.*
2. **Role matching** (law 3, role beats load). `sched_class × cpu_role`. In F3-3 collapsed
   into the pin (cpu1hello = implicit PINNED_RT, cpu1 = PINNED_WORKER).
3. **Pressure / balancing** (load). Least-loaded legal+role-matched CPU. **NO-OP in F3-3**
   (`choose_cpu` step 3 returns CPU0). F3-7 inserts one branch here; no caller changes.
4. **Adaptive priority** (the MLFQ, UNCHANGED). The existing 140-level active/expired
   runqueue + nice + yield_boost. The policy layer never reaches in. *Wrap, do not replace.*

The wrap contract: **layers 1–3 choose a CPU; layer 4 (the untouched MLFQ on that CPU)
chooses the task.**

## The seam today (F3-3, ~12 lines, layers 1–2 live, 3 stub, 4 untouched)

```
scheduler_choose_cpu(p):
  1. pinned_cpu valid + online + in-mask  -> return pinned_cpu   (legality; pin wins)
  2. allowed has cpu1 & not cpu0 & online -> return 1            (legality; F3-3 cpu1-only branch)
  3. return 0                                                    (home; NO balancing)
```

`cpu1hello` reaches CPU1 **only** because its spawn helper set `pinned_cpu=1` +
`allowed_cpus=(1<<1)` and the seam reads those generic fields — never by name. Normal tasks
(`allowed_cpus=(1<<0)`, `pinned_cpu=CPU_NONE`) fall to step 3 → CPU0, exactly as today.

## The seven laws (the policy layer's constitution; lower number wins a conflict)

1. **Legality before optimization** — only ever placed on a legal CPU.
2. **Pinning beats balancing** — a pinned task goes to its pin; the balancer may not move it.
3. **Role beats load for specialized work** — specialized class → matching CPU role.
4. **Interactive latency protection** — UI/compositor/input not starved by batch.
5. **Batch fills idle** — batch runs on spare capacity, backs off under pressure.
6. **Recovery outranks normal** — watchdog/self-heal gets CPU so a wedged system recovers.
7. **No CPU trusted blindly** — a CPU failing its invariants (raised
   `g_sched_invariant_violations`, stalled heartbeat) is quarantined out of the legal set.

F3-3 actively obeys 1/2/3/7 and reserves 4/5/6 as no-ops it must not violate. Law 7 is
*why* the race-free validator is part of F3-3: an adaptive scheduler that cannot trust a CPU
must be able to **detect** an untrustworthy one, and that detection must itself be race-free.

## Roadmap (each brick wraps the prior; none rewrites the MLFQ; each hard-gated)

- **F3-3** — `choose_cpu` seam + F3-2 fields + race-free all-`rq_lock` validators; first
  pinned ring-3 process on CPU1. Gate: `CPU1HELLO: PASS` + RQLOCK/AFFINITY + 0 `[SCHED_INVARIANT]`.
- **F3-4** — AP syscall/exit/reap lifecycle: **per-CPU "current" resolution** at the syscall
  boundary + an AP-safe exit/schedule path (no global write, no PIC EOI). *Prerequisite for
  cpu1hello's exit — see the verifier finding below.*
- **F3-5** — adaptive scoring (layer 4 additive, single-CPU): interactive credit / hog decay.
- **F3-6** — land `sched_profile_t`/`sched_class_t`/`cpu_role_t`; relocate the loose fields
  into `p->sched`; `scheduler_submit_task()` born as the named placement funnel.
- **F3-7** — controlled migration / balancing (layer 3 live): harvest `scheduler_smp.c`'s
  `MIGRATION_COST_THRESHOLD`/`LOAD_IMBALANCE_THRESHOLD` algorithm, rebind onto the F3-1
  `cpu_t` MLFQ, retire the orphan `cpu_runqueue_t`. Both `rq_lock`s ascending (F3-3a armed it).
  Only non-pinned NORMAL/BATCH migrate (laws 2,3).
- **F3-8** — IPI reschedule (remote wake after cross-CPU enqueue/migration).
- **F3-9** — pressure counters + auto-quarantine (closes law 7).
- **F3-10** — TLB shootdown for address-space changes under migration.
- **F3-11** — class-gated work stealing: `pick_best_task_for_cpu` (local pick first, steal a
  BATCH-class cache-cold task as fallback). INTERACTIVE/RECOVERY/PINNED never stolen.

## The submit-task pipeline (incremental, no flag day)

```
scheduler_submit_task(p):
  1. mask   = scheduler_legal_mask(p)         // layer 1
  2. target = scheduler_choose_cpu(p)         // layers 2-3
  3. assert (mask >> target) & 1              // policy can't escape legality
  4. scheduler_add_process_to_cpu(p, target)  // enqueue into target CPU's MLFQ (the fixed sink)
```

F3-3 runs this by hand for one task (steps 1+3 collapsed into the existing enqueue gate).
F3-6 names it `scheduler_submit_task`; `scheduler_add_process` (self-enqueue) becomes a thin
shim. Every brick only **prepends** policy in front of a proven, gated enqueue sink.

## Anti-goals for F3-3 (enforced, not aspirational)

No load balancing (step 3 returns CPU0). No work stealing. No automatic migration. No general
userspace on CPU1 (exactly one hello-then-exit app). **No name-based dispatch** (the single
most important one — it is what makes F3-3 step 1 of a real policy layer, not a demo hack).
No MLFQ rewrite. No preemption (cpu1hello is cooperative). No class/role/pressure types yet
(F3-6). No auto-quarantine action yet (F3-3 builds only the detection prerequisites).

## Verifier finding that resequenced the bricks (2026-06)

The F3-3 adversarial panel REJECTED the cpu1hello *exit/reap* half: the AP syscall boundary
resolves "current" via the **global** `current_process`, which on CPU1 names **CPU0's** task.
So `sys_exit` on CPU1 would mark the wrong (BSP) process — likely init — TERMINATED, call the
BSP `schedule()`/`process_set_current()` from the AP (writing the global, forbidden), and leak
cpu1hello as an unreapable zombie — while printing `CPU1HELLO: PASS` first (false green).
Therefore **F3-4 (per-CPU current resolution + AP-safe exit) is a prerequisite for cpu1hello's
exit** and must land before the full F3-3b. The iretq entry, the `rq_lock`-not-across-switch
discipline, and CPU0-can't-run-the-pinned-task were all verified SAFE. F3-3a (the race-free
validator prep) is independently green and lands first.
