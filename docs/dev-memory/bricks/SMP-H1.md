# brick record: SMP-H1 (BKL-LITE)

> The Linux-2.0-style safety wall before BATCH work runs real userspace on
> CPU1 (the user: "the safety brick that lets F3-7 become useful instead of
> reckless"). One owner-recursive outer kernel lock around the marked-unsafe
> syscall groups, proven by a 60-second two-CPU storm: ~160k marked-syscall
> rounds with per-iteration corruption detection, zero errors, zero
> deadlocks, the whole ladder green underneath.

```yaml
brick: SMP-H1
status: complete
branch: brick/smp-h1-bkl-lite
base: brick/smp-profile-0 (dae9f75, the frozen PROFILE-0 head)
commits: [d921238]
acceptance: "BKL: PASS syscall_storm=1 duration=60s cpu0=1 cpu1=1 corruption=0 deadlock=0 panic=0"
proof: scripts/bkl_smoke.sh (ladder profile + SMP_BKL=1, qemu -smp 2, ~300s)
measured: CPU1 storm 157,757 iters / CPU0 storm 4,544 iters (sharing the desktop), both errors=0 secs=60; engaged cpu0_acq=1 cpu1_acq=8 contended=1
```

## the lock (kernel/core/syscall/bkl.c)

- **Owner-recursive BY CPU**: on this cooperative kernel a task cannot lose
  its CPU mid-syscall without blocking, and blocking paths are unmarked by
  construction -- so CPU identity IS ownership. Re-entry by the owning CPU is
  depth++, not self-deadlock.
- **Acquired in the dispatcher** (syscall.c) around whole handler bodies,
  falls through (no early return) so post-dispatch diagnostics stay live.
  The getpid/get_ticks/yield fast paths bypass the table entirely -- never
  wrapped, per the brick scope.
- **Marked groups**: FS (open/read/write/close/dir/stat/unlink/rename/mkdir/
  map_file) · IPC (shm, msgget/msgsnd/msgctl, clipboard, notify, all
  channels + spawn_ex) · NET non-blocking subset (socket/send/sendto/
  close_sk/bind/listen/net_info/net_send/net_recv/sock_poll) · PROC (fork/
  execve/spawn/kill/nice/priority/proclist/proc_query/proc_ctl/blk_read/
  blk_write).

## THE LOAD-BEARING EXCLUSION (why nothing marked may block)

Syscalls run IF=0 and the kernel is cooperative: a marked syscall that
context-switches away while holding the BKL leaves it owned by a parked
task; the other CPU spins IF=0 forever = a cross-CPU wedge. Linux 2.0 solved
this with release-on-block in schedule(); **BKL-LITE v1 refuses to mark
anything that can schedule away mid-syscall**: exit, waitpid, sleep, yield,
read_event, msgrcv, ioctl (PTY), recv/recvfrom, accept, connect, futex,
thread_exit/join, ch_wait. Audited before marking: sys_execve aliases
sys_spawn (returns normally) and sys_kill defers the dead-task switch to the
scheduler -- no marked handler can fail to release. If a future brick makes a
marked path blocking, the **2s bounded-spin watchdog** is the tripwire: a
loud one-shot `[BKL] possible deadlock` diagnostic -- never a silent hang,
never an unlocked proceed (visible wedge > silent corruption).

## the storm (userspace/tests/bklstorm.c)

Two instances spawned by kernel.c under SMP_BKL: CPU1-pinned PINNED_RT + a
default NORMAL the funnel homes to CPU0 (both placements through
scheduler_submit_task -- the PROFILE-0 funnel carrying real test traffic).
Each runs 60 wall-clock seconds (SYS_GET_TICKS_MS) of marked-group rounds:
open/read/close + stat + readdir, shm get/at/fill/VERIFY/dt churn (the
corruption detector AND the heap/slab stress), clipboard set/get (the two
storms deliberately race the shared resource), yielding every 64 iterations.
The CPU1/CPU0 iteration asymmetry (157k vs 4.5k) is the desktop sharing
CPU0 -- expected, and itself evidence both schedulers stayed live.

## what the acceptance flags actually prove

- `syscall_storm=1` -- both spawn markers + both completion lines
- `duration=60s` -- secs>=60 on both completion lines
- `cpu0=1 cpu1=1` -- the kernel-side `[BKL] engaged` counters: both CPUs
  executed marked paths under the wall (stronger than self-reported CPU ids;
  no getcpu syscall exists -- the F3-3b plan's SYS_GETCPU=89 was never
  landed, 89 went to NET_CONFIG)
- `corruption=0` -- errors=0 on both (the shm pattern verify) + 0 sched/tlb
  invariant violations under the churn ("verify heap/futex/rq locks under
  2-CPU stress")
- `deadlock=0` -- zero watchdog/release-bug diagnostics
- `panic=0` -- the usual fault scan + desktop alive

## byte-identity

Default kernel.elf `6f99ed9f` hash-equal. The non-BKL ladder profile
contains zero H1 code (every kernel change is `#ifdef SMP_BKL`; its baseline
is now recorded: `213d960b`). bklstorm ships in every initrd (tiny, inert
without the gated spawn -- the cpu1hello convention); the initrd grew by one
file, which is additive.

## boundary held (user-set hard no's)

No desktop split · no work stealing · no BATCH migration · no general
per-mm shootdown · no global PREEMPT · no fine-grained VFS rewrite.

## next

SMP-F3-7 BATCH-CLASS / controlled CPU1 placement (the user's stated order;
the wall this brick built is what makes it non-reckless), then DESKTOP-SPLIT.
Future BKL evolutions when needed: release-on-block (lets blocking syscalls
be marked), then per-subsystem lock splitting (the Linux 2.2 path).
