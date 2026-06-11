# brick record: SMP-R0 (F3-4 recovery)

> Track 2's opening brick: recover the validated-but-stranded F3-4 work
> (per-CPU "current") instead of rewriting it blind, mine the old stash, and
> tell the truth about the stale SMP docs. En route it surfaced that EVERY
> SMP build had been silently link-broken for two days.

```yaml
brick: SMP-R0
status: complete
branch: brick/smp-r0
base: brick/net-p1-0 (8b446aa)
checkpoints:
  - id: cherry-pick
    commits: [2bfeecc]
    result: >
      git cherry-pick faf7444 (from smp/f3-4-ap-current; NEVER merge -- the
      branches diverged ~42k lines). 4 conflicts resolved as HEAD + the
      documented hunks. ONE ADAPTATION: this tree had inlined
      process_get_current() as a hot-path single MOV (sched.h static inline),
      so the F3-4 routing is #ifdef'd -- under SMP_SCHED_DISPATCH it resolves
      cpu_get_current_thread() (per-CPU); default/SMP_FOUNDATION builds keep
      the global-load MOV (byte-identical, and the divergence only exists
      when CPU1 can run tasks). Also: the F2 VERIFY + APCURRENT gate lines
      use kprintf, NOT BOOT_LOG (suppressed by the always-on -DBOOT_QUIET --
      the smoke-drift class), and APCURRENT sits at the REAL post-spawn F2
      VERIFY site (kernel.c ~1297), not the earlier pre-spawn heartbeat block.
  - id: link-breakage fix
    commits: [67422d0]
    result: >
      First dispatch-mode build on this lineage exposed: kernel_panic calls
      ipi_stop_all_cpus under bare SMP_FOUNDATION (added in c70ee87, June 8)
      but NO ipi*.c is in quick_build's compile list -- EVERY SMP=1 build
      since c70ee87 link-failed, unnoticed because SMP builds are rare (and
      quick_build exits 0 on link failure -- grep the log, never trust rc).
      Fix: gate on SMP_FOUNDATION && SMP_IPI; SMP-G0 (IPI-LINK) compiles
      ipi.c, defines SMP_IPI, re-enables the stop. Until then a panicking BSP
      doesn't stop CPU1 (pre-c70ee87 behavior; CPU1 = bounded coprocessor).
  - id: stash triage
    result: >
      stash@{0} ("WIP on debug/slab-churn-corruption") is now MINED (keep as
      archive, never pop). Verdicts: futex.c IDENTICAL to HEAD (the
      coordination-layer work fully landed). ipi.c: HEAD's version SUPERSEDES
      the stash's older copy -- nothing to mine. HARVEST ITEMS RECORDED:
      (1) ipi_handlers.asm has a stash-only ipi_tlb_flush_page_handler
      (vector 0x47, single-page shootdown) -> harvest at SMP-G2/G3;
      (2) syscall.asm has a stash-only optimised SYSCALL_BODY (callee-saved
      registers not pushed/popped; ~12 cycles + 48 B stack per syscall) ->
      optional later perf brick. The bulk userspace deltas belong to the dead
      debug-branch desktop and are superseded by the brick-chain lineage.
  - id: docs honesty
    result: >
      docs/SMP_COMPLETION_REPORT.md (gitignored/untracked) got a working-tree
      banner: STALE/OVERCLAIMING -- "written" != "integrated+proven";
      SMP_SCHEDULER_PLAN.md + SCHEDULER_POLICY_LAYER.md stay authoritative.
acceptance:
  - "cpu1_smoke.sh (SMP=1 SMP_SCHED=1 SMP_SCHED_DISPATCH=1, qemu -smp 2):
     PASS -- Brick F2 VERIFY delta=8063148 + APCURRENT: PASS (cpu1
     process_get_current() is cpu1-local + distinct from cpu0) + 0
     [SCHED_INVARIANT] + 0 panic. THE F3-4 boundary proof, live on CPU1."
  - "default build: 0 errors (the per-CPU routing is compiled out; hot path
     unchanged)."
  - "smp_smoke.sh: substantive foundation GREEN in serial (CPU1 up via
     INIT-SIPI-SIPI, dual-CPU matmul split verify OK at 1.94x speedup) but
     the script's GRADING is stale drift -- it builds SMP=1 (FOUNDATION-only)
     yet grades RQLOCK/AFFINITY, which are SMP_SCHED-only selftests that can
     never print in its own build; folded into SMOKE-PROFILE-0. It also
     could not even LINK from c70ee87 until 67422d0."
verdict: pass
done: >
  F3-4 is recovered, adapted, and proven on this lineage: a CPU1 context
  resolving "current" gets CPU1's task. The prerequisite for cpu1hello
  (SMP-F3-5) is in place, the stash is closed out, and the SMP build links
  again.
next:
  - SMP-F3-5 cpu1hello: first ring-3 process on CPU1 (high-risk brick; exit-
    path AP audit + the running-ref process_unref fix land WITH it per the
    verifier-mandated split documented in faf7444's message).
  - SMP-G0 IPI-LINK: compile ipi.c (+ ipi_handlers.asm), define SMP_IPI,
    check the link map < 0x200000 (the .bss user-shadow hazard), one
    IPI_RESCHEDULE round-trip.
```
