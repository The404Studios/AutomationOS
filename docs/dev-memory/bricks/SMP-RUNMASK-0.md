# brick record: SMP-RUNMASK-0

> The audit micro-brick before DESKTOP-SPLIT (the user: "the audit needs to
> be correct before the desktop starts depending on it"). The TLB pin audit
> now audits EXECUTION REALITY -- per-CR3-aggregated ran_on_cpus, stamped at
> the dispatch chokepoints -- instead of the stale declared-mask heuristic
> that F3-7's batchdemo made too crude.

```yaml
brick: SMP-RUNMASK-0
status: complete
branch: brick/smp-runmask-0
base: brick/smp-f3-7-batchclass (c5fd478, the frozen F3-7 head)
commits: [16482d8]
acceptance: "RUNMASK: PASS declared_multimask_ok=1 actual_single_cpu=1 forced_crosscpu_detected=1 tlb_neg_valid=1"
proof: scripts/runmask_smoke.sh (FULL stack + SMP_RUNMASK=1, qemu -smp 2)
```

## what landed

- **`p->ran_on_cpus`** (gated SMP_RUNMASK, end-of-struct with p->sched):
  bit N set the first time the task dispatches on CPU N. memset(0) = "never
  ran" (correct default). Stamped at BOTH dispatch chokepoints --
  `process_set_current` (every BSP dispatch) + `ap_cooperative_schedule`'s
  switch commit (CPU1's only dispatch path).
- **`runmask_audit_crosscpu()`** (ipi.c, low .bss scratch): aggregates
  ran_on_cpus PER CR3 across live processes -- threads share an mm, so the
  ADDRESS SPACE is the audited unit, not the PCB. Kernel-CR3 residents
  (idle threads, kthreads) are exempt: the kernel space legitimately runs on
  every CPU and is exactly what G2's kernel-range shootdown protects.
  Violations are loud `[RUNMASK] VIOLATION` lines + a count.
- **Exit-boundary recording** (`process_on_terminate`): dying MULTIMASK
  processes log `allowed= ran= single_cpu=` + counters. Load-bearing:
  batchdemo lives ~50 ms -- a live walk can never see it; the exit record is
  where the brick's central evidence comes from:
  `[RUNMASK] exit record: 'batchdemo' allowed=0x3 ran=0x2 single_cpu=1`.
- **The upgraded NEG**: `TLBSHOOT_NEG: PASS no_user_crossflush_needed_under_
  pinning=1` keeps its exact prefix (every frozen smoke's grep -qF stays
  true) with `(RUNMASK upgrade: ... declared multimask OK)` semantics --
  declared multi-masks are fine; actual cross-CPU mm execution fails.
- **`runmask_selftest()`** -- the forced case: PLANT a cross-CPU footprint
  on a live PCB (init), assert the audit detects it (+1 violation, loudly),
  restore, assert clean. One mm never actually runs on two CPUs. Exactly ONE
  `[RUNMASK] VIOLATION` line in the whole acceptance boot = the planted one.

## en-route find: THE __LINE__-SHIFT TRAP (the keeper)

The first hook placement -- inside `cpu_set_current_thread`
(scheduler.c:181) -- changed the DEFAULT kernel hash even though the hook
was `#ifdef`'d out. Cause: the preprocessor counts lines in FALSE ifdef
branches, so the 9 inserted lines shifted the `__LINE__` values embedded by
the `ASSERT_ALWAYS` calls at scheduler.c:321-345 below the insertion. The
object-level bisection (md5sum per .o, diff the two builds) found it in
minutes: only c_scheduler.o differed. RULE: a default-compiled file with
__LINE__ users tolerates gated insertions only BELOW its last __LINE__
site. Both stamps relocated below; default `6f99ed9f` hash-equal
re-verified; the full ladder re-proven after the move.

## byte-identity

Default `6f99ed9f` hash-equal (after the __LINE__ fix). Every change is
`#ifdef SMP_RUNMASK`; the BATCH profile contains zero RUNMASK code.

## boundary held

No scheduler policy change (the stamps observe, never route) · no desktop
split. The G2 mask heuristic remains in place for non-RUNMASK builds.

## next

DESKTOP-SPLIT -- the forcing function it will lean on is now armed and
audits reality: the day any address space actually executes on two CPUs
without per-mm shootdown work, the smoke fails loudly.
