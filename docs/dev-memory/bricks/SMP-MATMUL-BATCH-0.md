# SMP-MATMUL-BATCH-0 — a real threaded CPU-heavy workload runs wholly on CPU1

**Status:** FROZEN / COMPLETE. Branch `brick/smp-matmul-batch-0` off
`brick/smp-thread-inherit-0` HEAD `e4c7c2d`. Commits (unsquashed): `13f671c`
feat + the docs commit. Default kernel byte-identical to HEAD (`32f2f69c`).

## Why this brick
SMP-THREAD-INHERIT-0 proved the inheritance predicate: a thread created via
`SYS_THREAD_CREATE` inherits its leader's `mm_place->home_cpu` + `sched_class`
with a **narrowed** single-CPU `allowed_cpus`, so one shared address space only
ever executes on one CPU (until per-mm TLB shootdown exists). That made it safe
to put the first real **threaded** CPU-heavy workload on the BATCH allowlist:
`sbin/matmuljobs` (a float matmul partitioned into 8 contiguous row-band jobs,
drained by 2 worker threads through the userspace job queue, result compared
bit-for-bit against a single-threaded reference).

## What landed (all `#ifdef SMP_DSPLIT`)
`kernel/core/syscall/handlers.c`:
- `dsplit_allow[]` gains `"matmuljobs"` (was `{batchdemo, bklstorm}`).
- The membership comment is rewritten for the post-THREAD-INHERIT rule:
  threaded apps are eligible **only** because their workers inherit the leader's
  home CPU + class (no blanket "all threaded on CPU1"; do not add arbitrary
  threaded apps).
- `dsplit_maybe_route()` emits a `[MATMULBATCH]` audit probe (name, home_cpu
  chosen by the seam, allowed mask, class) when the allowlisted name matches
  `matmuljobs`.

`sbin/matmuljobs` is already spawned by init (committed); no kernel spawn change.

## The proof
`build_test/smp_matmul_batch_smoke.sh` — full SMP stack + `SMP_DSPLIT` +
`SMP_THREAD_INHERIT`, one `-smp 2` boot, gates keyed to the **actual** kernel
serial strings:

```
SMP-MATMUL-BATCH-0 PASS desktop_cpu0_only=1 batch_matmul_cpu1=1
  thread_inherit_home_cpu=1 shared_mm_single_cpu=1 runmask_violation=0
  default_byte_identical=1
```

Load-bearing lines (authoritative 300s run):
```
[DSPLIT] 'sbin/matmuljobs' PID 34 -> BATCH allowlist, the seam chose cpu1
[MATMULBATCH] allowlisted: name=sbin/matmuljobs pid=34 home_cpu=1 allowed=0x3 class=BATCH
matmuljobs: PASS result-matches-ref jobs=8/8 workers=2
[RUNMASK] exit record: pid=34 'sbin/matmuljobs' allowed=0x3 ran=0x2 single_cpu=1
[THREADINHERIT] summary: batch_parent_cpu1=1 workers_same_cpu=1 sched_inherit=1 mm_single_cpu=1 workers=2 (cpu1=2 cpu0=0)
```
Plus every desktop-core proc observed `ran=0x1` (compositor/terminal/filemanager/
netman/browser/derby/init/reaploop), and all inherited SMP walls green:
TLBSHOOT_NEG, 2 BKL storms (errors=0), IPI wake 32/32, IPILINK, TLBSHOOT; 0
SCHED+TLB invariant; default kernel md5 `32f2f69c`.

### Evidence design (why these are the right gates)
- matmuljobs is **short-lived** (runs the matmul, prints one PASS line, exits),
  so the LIVE runmask walk can never see it — the proof of "ran on exactly one
  CPU" is its **exit record** (`process.c`, fires for any dying process whose
  `allowed_cpus` has >1 bit): `allowed=0x3 ran=0x2 single_cpu=1`.
- Its worker threads inherit a **narrowed** `0x2` mask (single bit → no exit
  record, but pinned to CPU1 by construction).
- The general inheritance predicate is proven by the persistent `threadprobe`
  `[THREADINHERIT] summary` (hard-coded to threadprobe in `health_monitor.c`);
  matmuljobs reuses the identical `SYS_THREAD_CREATE` path.

## The proof vehicle was rewritten (the draft was broken)
The uncommitted draft smoke would have proven nothing:
1. Built **without** `SMP_SCHED/SCHED_DISPATCH/IPI/BKL` → CPU1 never dispatches.
2. Ran `build_all.sh`, which packs the **default** kernel into the ISO → booted
   the wrong kernel.
3. Gated on literals the kernel never prints (`runmask_clean=1`, `33/33`).
4. Regression block sat after `exit` → dead code.

Rebuilt on the proven `threadinherit_smoke.sh` scaffold: full stack build, swap
`kernel-smp.elf` into the ISO (restore default after), LINK gate, byte-identity
md5, real-string gates, smoke_boot run for information only.

## Three en-route finds (keepers)
1. **Stale byte-identity anchor.** The pre-fork SMP bricks anchored on
   `6f99ed9f`, but fork/execve (`f4e9420..e4c7c2d`) legitimately changed the
   DEFAULT kernel. True HEAD default = `32f2f69c` (verified by a clean-HEAD
   worktree build). This brick is all `#ifdef SMP_DSPLIT` and handlers.c has no
   `__LINE__` users, so the default stays byte-identical — and **byte-identity
   IS the regression proof** (an unchanged default kernel has, by construction,
   the identical smoke_boot result as HEAD). smoke_boot is run only for info.
2. **Kernel fault ≠ handled user fault.** An SMP *placement* brick proves the
   KERNEL stays healthy, not that every unrelated user test app is bug-free. The
   `kern_fault` gate counts only FATAL kernel (CPL=0) faults / triple faults /
   kernel panics / a CRITICAL proc killed by an exception. A gracefully-handled
   CPL=3 user `#UD` (kernel terminates the bad process, system continues) is
   correct behavior, reported separately as `handled_user_faults`.
3. **Pre-existing `sbin/sigtest` #UD (flagged).** HEAD's plain default boot
   already throws ONE `Invalid Opcode (vector 6)` at user RIP `0x800079` in
   `sbin/sigtest`; the kernel cleanly terminates it and continues. Present in
   B7-era soak logs — **not** a fork/execve or matmul regression. It is the lone
   `handled_user_faults=1` and is why the default `smoke_boot` scores 36/43 (the
   other 6: 4 SMP-only/self-test markers absent in the default profile + 2 benign
   CoW page-faults from the fork proof apps). **Candidate follow-up: SIGTEST-UD-0**
   (root-cause the user `#UD`; kernel side is robust).

## Honesty note on the acceptance line
The draft listed `smoke_boot=33/33`; that literal is unreachable (smoke_boot now
has 43 checks and HEAD itself scores 36/43 from pre-existing issues). The real,
stronger regression proof is `default_byte_identical=1`, so the printed
acceptance uses that.

## HARD NO's held
Desktop / compositor / normal apps stay CPU0 · only explicit allowlist entries
get the multi-CPU BATCH mask · threads inherit the address-space home CPU (never
widen) · a shared mm never runs on two CPUs · no work stealing · no general
migration · no per-mm shootdown · no global PREEMPT.

## Queued behind it (user-set)
SMP-CPU1-PREEMPT-0 (CPU1-local BATCH preemption only) · SMP-PERMM-TLBSHOOT-0
(real per-mm remote invalidation, retires the TLBSHOOT_NEG pin assumption) ·
SIGTEST-UD-0 (the pre-existing user #UD above).
