# brick record: DESKTOP-SPLIT-0

> The first SMP milestone where the desktop stops merely PROVING CPU1 and
> starts USING it. An ordinary userspace `sys_spawn` child, declared BATCH by
> a boring explicit allowlist, runs its whole life on the second core while
> the compositor/input/shell stay pinned to CPU0 -- and every SMP safety wall
> built across the G0..RUNMASK ladder stays green through a 30-minute soak.

```yaml
brick: DESKTOP-SPLIT-0
status: complete
branch: brick/desktop-split-0
base: brick/smp-runmask-0 (7e458fb, the frozen RUNMASK head)
commits: [0eb41ed (feat), <docs>]
acceptance: "DESKTOPSPLIT: PASS cpu0_desktop=1 cpu1_batch=1 fps_within_tolerance=1 runmask=1 tlb_neg=1 bkl=1 soak=30m panic=0 invariant=0"
proof: scripts/dsplit_smoke.sh (full stack + SMP_DSPLIT=1, qemu -smp 2, baseline boot + 30-min soak)
profile: SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1 SMP_IPI=1 SMP_BKL=1 SMP_BATCH=1 SMP_RUNMASK=1 SMP_DSPLIT=1
```

## what this brick claims (and does NOT claim)

- **CLAIMS:** allowlisted, single-threaded BATCH work runs on CPU1 through
  typed intent (`sys_spawn` -> allowlist -> `scheduler_submit_task` -> the
  F3-7 choose_cpu batch branch -> CPU1), the desktop core stays CPU0-only in
  EXECUTION REALITY (not just declaration), and the split does NOT materially
  hurt desktop responsiveness, with every SMP safety wall green over 30 min.
- **DOES NOT CLAIM** an FPS speedup. The three soaks disagree on direction
  (run1 split faster, run2 slightly slower, run3 slightly slower) and the
  spread is host/QEMU side, not the split. See the FPS section.

## the five load-bearing evidence lines (run 3)

1. **sys_spawn origin** -- `[INIT] Spawning batchdemo...` ->
   `[SPAWN] Spawning 'sbin/batchdemo'` -> `Process 'sbin/batchdemo' created
   with PID 26`. A userspace child of init, created by the spawn syscall.
   (Wording note, user-set: this is the *userspace sys_spawn proof*, not a
   hand-clicked desktop-origin action -- and that is exactly the seam this
   brick proves: sys_spawn -> allowlist -> submit -> CPU1.)
2. **BATCH by allowlist** -- `[DSPLIT] 'sbin/batchdemo' PID 26 -> BATCH
   allowlist, the seam chose cpu1`.
3. **the seam routed to CPU1** -- same line ("the seam chose cpu1"); the
   choice came from `scheduler_submit_task`'s F3-7 batch branch, not a pin.
4. **actual ran_on_cpus = 0x2** -- `[RUNMASK] exit record: pid=26
   'sbin/batchdemo' allowed=0x3 ran=0x2 single_cpu=1`. A declared-multimask
   process that ACTUALLY executed on exactly CPU1 -- the RUNMASK audit's
   execution-reality evidence, not a declared mask.
5. **desktop core actual ran_on_cpus = 0x1** -- all six desktop-core procs
   (`compositor`, `terminal`, `filemanager`, `netman`, `browser`, `ide`)
   observed `ran=0x1`; ZERO desktop-core procs observed on CPU1.

## what landed (all #ifdef SMP_DSPLIT)

- **`kernel/core/syscall/handlers.c`** -- the boring allowlist
  `dsplit_allow[] = { "batchdemo", "bklstorm" }` + `dsplit_basename_match` +
  `dsplit_maybe_route`: on the sys_spawn success path (before first dispatch,
  so run history starts clean) an allowlisted child is dequeued, declared
  `allowed_cpus=0x3 pinned_cpu=CPU_NONE sched_class=BATCH`, and re-placed via
  `scheduler_submit_task` -- the same dequeue->declare->submit pattern the
  kernel-spawned CPU1 workloads use. matmuljobs EXCLUDED with a loud comment.
- **`kernel/core/health_monitor.c`** -- a one-shot `[DSPLIT] observed:` walk
  (~sample 12, ~60s) printing each sbin/* + CPU1 runner's `ran=0x%x` execution
  history + a count line: the cpu0_desktop / cpu1ran evidence.
- **`userspace/init/main.c`** -- init spawns `sbin/batchdemo` (the userspace
  sys_spawn trigger; the kernel-spawned F3-7 batchdemo PID 6 also appears).
- **`userspace/compositor/compositor_m8.c`** -- `[COMP] fps window` print
  every 30s off the existing g_fps_x10 IIR (the FPS measurement) + the
  icon-ghost create cooldown bumped 3->24 frames (carried from FORGE).
- **`scripts/quick_build.sh`** -- the SMP_DSPLIT gate.
- **`scripts/dsplit_smoke.sh`** + `build_test/_dsplit_{runner,verdict}.sh` --
  the two-phase proof harness (baseline -smp 1, split -smp 2 30-min soak).

## the FPS gate: tolerance-banded median (user-set, replacing max-vs-max)

The original `fps_ge_baseline` gate (max split window >= max baseline window)
was too brittle at a CAPPED ~10fps compositor: run 2 failed at split max 99 vs
baseline max 100 -- ONE measurement quantum (9.9 vs 10.0 fps) inside host
jitter, not a real regression. Evidence it was noise: run-to-run baseline max
swung 89..100 on the SAME kernel, and run 2 had a 13-minute mid-soak dip to
~7.5fps that correlated with NOTHING in the guest (no spawn/storm/SMP event),
then recovered. The replacement, user-approved:

    fps_within_tolerance = split_first9_median >= 90% of baseline_first9_median

Median over the first 9 fps windows of each boot = matched boot-relative
periods; the band absorbs the host/QEMU jitter. Measured:

    run 1: split appeared FASTER  (the early max-vs-max read: 98 vs 89)
    run 2: split 93 vs baseline 98 = 94.9%  PASS
    run 3: split 95 vs baseline 97 = 97.9%  PASS

Honest interpretation (user-set): run 1 appeared faster, run 2/3 appeared
slightly slower, and the variance was host-side. DESKTOP-SPLIT-0 does not
claim an FPS improvement; it claims the split keeps the desktop WITHIN
TOLERANCE while moving allowlisted BATCH work to CPU1. That is stronger
engineering than pretending a green run proved a speedup.

## the walls that stayed green (the inherited ladder, 30-min soak)

RUNMASK-CORE PASS + exactly ONE `[RUNMASK] VIOLATION` (the planted selftest --
real traffic added zero) · TLBSHOOT_NEG PASS · both BKL 60s storms errors=0 +
`[BKL] engaged` + 0 deadlock · IPIWAKE 32/32 · IPILINK · TLBSHOOT kernel_flush
· 65 fps windows, desktop alive at the end, 0 PANIC, 0 SCHED+TLB invariant.
Every wall (G1 wake, G2 kernel-range shootdown, H1 BKL, F3-7 BATCH routing,
RUNMASK-0 execution audit) was built for exactly this brick and held under it.

## byte-identity

Default `6f99ed9ffaf09a7fcb36996324c9450b` hash-equal. Every change is
`#ifdef SMP_DSPLIT`; the two source comment corrections (handlers.c, init/
main.c -- "userspace sys_spawn proof" not "desktop-origin") were line-neutral
and the handlers.c one lives inside the gated block, so no __LINE__ shift
(law 19) and the default build is bit-for-bit unchanged.

## proof-path history (3 soaks, the honest record)

- **run 1** (33-min): everything green EXCEPT allow=0 -- nothing triggered the
  sys_spawn seam (the allowlisted apps were kernel-spawned, bypassing it). Fix:
  init now spawns sbin/batchdemo. Not a split failure; a proof-path gap.
- **run 2** (33-min): all five split items proven; FAILED only the brittle
  max-vs-max fps gate (99 vs 100). Drove the gate redefinition above.
- **run 3** (33-min): the committed proof vehicle (dsplit_smoke.sh, now
  carrying the revised median gate) produced the exact revised PASS itself --
  fps 97.9%, all walls green, all five items present, 0 panic/invariant.
- **operational note:** a detached `nohup` runner was silently torn down once
  when the WSL2 VM idled out (PID 27 = fresh boot) -- nohup survives SIGHUP,
  not VM teardown. The reliable launch is a single blocking `wsl.exe` call that
  holds the VM open for the whole soak.

## boundary held / parked

No work stealing · no arbitrary migration · no global PREEMPT · no per-mm (G3)
shootdown · matmuljobs stays OFF the allowlist. Parked, in order: thread
placement INHERITANCE (the prerequisite before any threaded app -- matmuljobs
included -- can ever join the allowlist) · G3 per-mm shootdown (behind the
RUNMASK audit) · COMPOSITOR-ICON-GHOST-0.

## next

Thread placement inheritance is the natural follow-on: it is the single gate
between "one allowlisted single-threaded app on CPU1" and "a real threaded
workload (matmuljobs) safely spanning both cores under the RUNMASK audit".
