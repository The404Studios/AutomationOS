# brick record: SMP-THREAD-INHERIT-0

> The brick that makes SHARED address spaces safe before more real workload
> lands on CPU1. A thread SHARES its parent's CR3, so it MUST run on the same
> CPU as the rest of that address space -- one mm, one execution CPU -- until
> per-mm TLB shootdown exists. Threads now INHERIT the parent's placement and
> PIN to it instead of taking the conservative CPU0-only ctor default; the
> shared mm run-history is the cross-CPU detector. This is the gate before any
> threaded app (matmuljobs) can join the BATCH allowlist.

```yaml
brick: SMP-THREAD-INHERIT-0
status: FROZEN+PUSHED (origin/brick/smp-thread-inherit-0 @ d3a0b78, ls-remote verified)
branch: brick/smp-thread-inherit-0
base: brick/desktop-split-0 (db795bf, the frozen DESKTOP-SPLIT-0 freeze head)
commits: [fc62123 (feat), d3a0b78 (docs + law 20)]
acceptance: "THREADINHERIT: PASS batch_parent_cpu1=1 workers_same_cpu=1 sched_inherit=1 runmask_clean=1 desktop_cpu0=1 matmuljobs_ready=1 no_allowlist_expansion=1 soak=30m panic=0 invariant=0"
proof: scripts/threadinherit_smoke.sh (full stack + SMP_DSPLIT + SMP_THREAD_INHERIT, qemu -smp 2, 30-min soak)
profile: SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1 SMP_BKL=1 SMP_BATCH=1 SMP_RUNMASK=1 SMP_DSPLIT=1 SMP_THREAD_INHERIT=1
```

## the hazard this brick closes

The address space here is `context.cr3` + a heap-shared `as_refcount` (no mm
struct). Before this brick `thread_create` gave every thread the F3-2
CONSERVATIVE default: `allowed_cpus = CPU0-only, pinned_cpu = CPU_NONE`. That
was safe only because no threaded app ran on CPU1. The moment a BATCH parent on
CPU1 spawned threads, those CPU0-masked threads would be enqueued on CPU1 by
`scheduler_add_process` (it uses `this_cpu()`), creating an affinity
CONTRADICTION (queued on CPU1, mask says CPU0) the validators would flag -- and,
worse, if a thread actually ran on CPU0 while the parent ran on CPU1, ONE
address space would execute on TWO CPUs. User mappings are local-`invlpg`-only
under the pin/no-migration assumption (G2/TLBSHOOT_NEG), so that split silently
breaks TLB correctness. The mechanical law: **one mm gets one execution CPU
until general per-mm shootdown exists.**

## what landed (all #ifdef SMP_THREAD_INHERIT)

- **`mm_placement {home_cpu, ran_on_cpus, sched_class}`** (sched.h) -- a SHARED
  descriptor allocated in `process_create` with the same lifetime as
  `as_refcount` (pointer-copied in `thread_create`, freed by the last AS user).
  home_cpu defaults 0 (CPU0); set to the leader's placement at
  `scheduler_submit_task`. End-of-struct, double-gated -> byte-identical layout
  for every pre-inherit build.
- **`sched_thread_inherit_placement(parent, t)`** (process.c) -- THE production
  predicate: a thread inherits the mm's home CPU and PINS to it
  (`allowed_cpus = 1<<home` -- NARROWED, never widened; `pinned_cpu = home`) and
  inherits the mm's class. `thread_create` calls it, OVERRIDING the CPU0-only
  default after sharing the mm descriptor. The threadinherit selftest calls the
  SAME function, so the proof cannot diverge from the production path.
- **Dispatch stamps** (process_set_current BSP + ap_cooperative_schedule AP)
  fold each dispatch into `mm_place->ran_on_cpus` via **`__atomic_fetch_or`**.
  ATOMIC by necessity: the per-PCB `ran_on_cpus` has no concurrent writer (a
  task is never on two CPUs at once), but the SHARED field CAN be written by
  CPU0 and CPU1 simultaneously -- which is EXACTLY the violation it detects. A
  plain read-modify-write could lose the other CPU's bit and HIDE the bug; the
  detector must be no weaker than the hazard (user-set, recorded as law 20).
- **`threadinherit_selftest`** (scheduler.c) -- the predicate on synthetic
  shells: a BATCH-CPU1 parent's thread MUST inherit CPU1-only + BATCH
  (matmuljobs_ready); a NORMAL parent's thread stays CPU0 (no desktop
  regression). Emits `THREADINHERIT-CORE: PASS`.
- **`threadprobe`** (userspace/tests, kernel-spawned in kernel.c) -- a threaded
  BATCH workload, spawned BATCH->CPU1 EXACTLY like batchdemo and pointedly NOT
  on the sys_spawn allowlist. Parent + 2 persistent worker threads loop
  cooperatively; the `[THREADINHERIT]` health-monitor observation reports the
  whole address space on CPU1.

## the six load-bearing evidence lines (the soak)

1. `[SMP] THREAD-INHERIT: threadprobe PID 7 class=BATCH unpinned -> the seam
   chose cpu1` -- **threadprobe is BATCH but NOT allowlisted** (kernel-spawned),
   and **the parent was placed on CPU1 by the scheduler profile**, not a pin.
2. `[THREADINHERIT] observed: worker pid=11 ran=0x2 class=1` / `pid=13 ran=0x2
   class=1` -- **the workers INHERITED CPU1 + BATCH**, they did not re-decide.
3. `parent 'threadprobe' ... mm_ran=0x2 ... mm_single_cpu=1` -- **the shared mm
   ran_on_cpus == 0x2**: the address space never spanned two CPUs.
4. `[DSPLIT] observed: ... 'sbin/compositor' ran=0x1` -- **the desktop core
   stays ran_on_cpus == 0x1** (CPU0).
5. `THREADINHERIT-CORE: PASS ... matmuljobs_ready=1` -- **matmuljobs is READY**
   (the inherit mechanism it needs is proven) **but NOT yet routed**.
6. `summary: batch_parent_cpu1=1 workers_same_cpu=1 sched_inherit=1
   mm_single_cpu=1 workers=2 (cpu1=2 cpu0=0)` -- the composite.

## byte-identity

Default `6f99ed9ffaf09a7fcb36996324c9450b` hash-equal (re-verified inside the
soak). Every change is `#ifdef SMP_THREAD_INHERIT`. __LINE__-safe: process.c /
handlers.c / kernel.c have no __LINE__ users; the scheduler.c edits all sit
BELOW its last `ASSERT_ALWAYS` (line 336) -- law 19 honored.

## proof-path history

Two runs. A 180s SHORT preflight proved every functional gate + all six
evidence lines green (only the soak-window count short, by design). Then the
full 30-min soak on the ATOMIC-detector kernel (rebuilt) hit the exact
acceptance: 64 fps windows, parent+workers all ran=0x2, mm_single_cpu=1,
runmask exactly one (planted) violation, all walls green, 0 panic / 0 invariant.
The atomic-OR detector fix landed BETWEEN the preflight and the soak (user
catch): the preflight's plain-|= kernel is identical in the happy path -- no
concurrent cross-CPU write occurs when inheritance works -- but the soak's
detector is now no weaker than the bug.

## boundary held / parked

No allowlist expansion · no work stealing · no general migration · no per-mm
shootdown · no global PREEMPT · no desktop policy expansion. matmuljobs stays
OFF the allowlist.

## next

**SMP-MATMUL-BATCH-0** -- the reward brick: with shared-mm safety proven,
matmuljobs (threaded) can finally join the BATCH allowlist; its worker threads
will inherit its CPU1 placement and the whole job runs wholly on the worker
core under the RUNMASK audit. Then SMP-CPU1-PREEMPT-0 (CPU1-local BATCH
preemption only) and SMP-PERMM-TLBSHOOT-0 (real per-mm remote invalidation,
which finally retires the TLBSHOOT_NEG pin assumption).
