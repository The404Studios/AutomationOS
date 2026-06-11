# brick record: SMP-F3-6 (CHOOSECPU)

> Back on the scheduler policy ladder: THE placement seam from
> docs/SCHEDULER_POLICY_LAYER.md made real. One decider answers "which CPU
> should this task run on?" -- legality first, pin/role second, home-CPU0
> stub third, the MLFQ untouched. Every CPU1 placement in the tree now routes
> through it.

```yaml
brick: SMP-F3-6
status: complete
branch: brick/smp-f3-6-choosecpu
base: brick/smp-g2-tlbshoot-min (33d8951, the frozen G2 head)
commits: [6508778]
acceptance: |
  CHOOSECPU: PASS pinned_cpu1=1 default_cpu0=1 illegal_clamped=1 nomask_clamped=1 multimask_home=1 cpu1only_role=1
  [SMP] F3-6: cpu1hello placed via scheduler_choose_cpu -> cpu1
proof: scripts/choosecpu_smoke.sh (same SMP_IPI dispatch profile, qemu -smp 2)
```

## what landed

- **`scheduler_choose_cpu(p)`** (scheduler.c, `#if SMP_SCHED && SMP_SCHED_DISPATCH`),
  the policy-layer spec verbatim:
  1. **Hard legality, never skipped**: `allowed_cpus ∩ online` (CPU0 always;
     CPU1 via `cpu1_is_online()` -- law 7's liveness input). Empty
     intersection (the F3-2 memset trap) or an off-mask pin = ILLEGAL ->
     CLAMPED loudly to a legal CPU; never returns an illegal target, never
     wedges the caller.
  2. **Pin/role** (law 2, pinning beats balancing): a legal pin wins; a
     CPU1-only legal mask routes to CPU1 even unpinned. Generic field reads,
     NEVER name-based dispatch.
  3. **Pressure/balancing -- DELIBERATE STUB**: home CPU0. No balancing, no
     migration; F3-7 inserts one branch here with no caller changes.
  4. The untouched MLFQ on the chosen CPU picks the task (wrap, not replace).
  ADVISORY by design: the mandatory F3-2 enqueue gate in
  `scheduler_add_process_to_cpu` stays the backstop.
- **One decider** (the F3-3 verifier panel's requirement): the F2 kthread
  spawn, the cpu1hello ring-3 placement, and every home-routed wake
  (`scheduler_add_process_home`'s inline ternary replaced) all route through
  the seam. cpu1hello's full ladder still passing IS the live proof the seam
  chooses identically to the hand placements it replaced.
- **`scheduler_choosecpu_selftest()`**: every branch on synthetic shells (the
  non-vacuous affinity-selftest pattern; one static process_t -- the struct is
  too big for the stack). The two CLAMPED warning lines in the boot log are
  cases 3/4 firing loudly, by design. Case 6 is reality-aware: CPU1-only
  unpinned expects CPU1 when CPU1 is online, CPU0 (legality clamp) on a
  single-core boot -- the selftest cannot false-FAIL a degraded boot.

## naming note (recorded at open)

The policy doc's own "F3-6" row (sched_profile_t/sched_class_t/cpu_role_t
types + the named scheduler_submit_task funnel) stays FUTURE; this brick
lands the SEAM (the doc's F3-3 spec) under the user's F3-6 name. Layers 1-2
live, 3 stubbed, 4 untouched.

## byte-identity

DEFAULT kernel.elf hash-identical (`6f99ed9f`). The SMP_SCHED_DISPATCH
profile changes BY DESIGN -- this is the first brick since F3-5 that targets
the dispatch path itself; the full regression ladder (TLBSHOOT+NEG, IPIWAKE
32/32, IPILINK, F2, APCURRENT, CPU1HELLO, 0 invariant, 0 panic) is its gate.

## boundary held (user-set hard no's)

No desktop split · no work stealing · no general migration (the layer-3 stub
cannot move anything) · no global PREEMPT · no per-mm shootdown (G3 stays
parked behind the G2 pin audit).

## next (the policy ladder, per the doc's roadmap)

- F3-7: controlled migration/balancing (layer 3 live) -- one branch in the
  stub; only non-pinned NORMAL/BATCH migrate; needs the G1 IPI kick (have it)
  and ascending two-rq_lock discipline (F3-3a armed it).
- Or the doc's typed F3-6 row (sched_profile_t + scheduler_submit_task).
- The G2 pin audit remains the forcing function for per-mm shootdown.
