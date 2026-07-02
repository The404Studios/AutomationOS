# AutomationOS — Modernization Phase 1

Status snapshot (2026-06-13). This documents the kernel/userspace features landed in the
Phase‑1 modernization pass (the "make on‑device cc reliable + POSIX depth + T410 speed"
plan) and how to build, use, and prove each. **Nothing here is committed yet** — all
changes live in the working tree; the cooperative default build is the safe baseline.

The default build is **cooperative**; `PREEMPT=1` builds the preemptive kernel
(`build/kernel-preempt.elf`). Build in WSL Arch: `bash scripts/quick_build.sh` (kernel)
then `bash scripts/build_all.sh` (userspace + ISO).

---

## 1. Real POSIX signals (B8 · SIG‑FULL‑0)

Per‑process signal handlers, masks, pending sets, default actions, and SIGCHLD.

**Syscalls** (`kernel/include/syscall.h`):

| # | name | args | returns |
|---|------|------|---------|
| 107 | `SYS_RT_SIGACTION`   | `(sig, handler, restorer)` — handler 0=SIG_DFL, 1=SIG_IGN, else user VA | 0 / −errno |
| 108 | `SYS_RT_SIGPROCMASK` | `(how, set*, oldset*)` — how 0=BLOCK 1=UNBLOCK 2=SETMASK | 0 / −errno |
| 109 | `SYS_RT_SIGRETURN`   | (none — restores the saved frame) | restored rax |
| 110 | `SYS_SIGPENDING`     | (none) | pending bitset |

**Mechanism** (`kernel/core/signal/kill.c`, `kernel/arch/x86_64/syscall.asm`):
`process_t` gains `sig_handlers[32] / sig_mask / sig_pending / sig_restorer` (zeroed by
`process_create`'s memset → SIG_DFL/empty). `syscall.asm` calls
`deliver_pending_signals(frame, retval)` *after* each syscall dispatch; it builds a
16‑byte‑aligned `ucontext` on the user stack (red‑zone‑skipped), redirects `sysret` into
the handler with `rdi=signo, rsi=ucontext`, and `SYS_RT_SIGRETURN` reverses it. Catchable
signals go **pending** in `sys_kill`'s default case (SIGKILL/STOP/CONT stay immediate +
uncatchable). A terminating child raises **SIGCHLD** on its real parent in
`process_on_terminate` (default disposition = ignore, so non‑handlers are unaffected).

**Mask/sigset convention:** bit `sig` (`1<<sig`), e.g. SIGUSR1=10 → bit 10. This matches
the kernel side and the raw‑syscall test; libc must use the same convention when wired.

**Two general bugs fixed via this work:**
- `copy_to_user` refuses read‑only **CoW** pages, so a handler could never be delivered to
  a freshly‑forked child (its stack is still CoW). Fixed by pre‑resolving CoW on the
  signal‑frame pages with `cow_handle_write()` before the copy.
- The saved‑frame pointer was a **global** (`g_sig_frame`) that every syscall overwrites,
  so delivery on the return of a **blocking** syscall (e.g. `waitpid`, which is how
  SIGCHLD arrives) built on a stale frame. Fixed: `deliver_pending_signals` uses the
  **local `rsp`** frame; `g_sig_frame` now serves only `sys_rt_sigreturn` (which never
  blocks). *Law: a signal can become deliverable while the target is blocked in a syscall;
  the delivery frame must be the local syscall frame, never a global.*

**Proof:** `sbin/sigtest` (raw syscalls, spawned by init) + `bash scripts/sig_smoke.sh`:
```
SIGFULL: PASS handler_runs=1 returns_clean=1 mask_blocks=1 pending_then_unblock=1 \
         default_action=1 bad_handler_failsafe=1 sigchld_on_exit=1
```
The bad‑handler case genuinely faults at `RIP=0x4000` in ring 3 and is contained to the
child; the kernel survives and the desktop comes up.

**Open:** `userspace/libc/signal.c` is still a **stub** (stores handlers, never calls the
kernel; `raise()` is synchronous; `sigprocmask`/`sigpending` are no‑ops). Wiring it is
deferred to the shell brick that will consume it — see §4.

---

## 2. poll/select + real epoll (B10 · POLL‑SELECT‑0)

**Syscalls:** `SYS_POLL=111` `poll(struct pollfd*, nfds, timeout_ms)` and
`SYS_SELECT=112` `select(nfds, readfds, writefds, exceptfds, timeval*)`
(`kernel/core/syscall/poll.c`).

**Unified readiness probe** `fd_poll_state(fd)` dispatches by precedence (there is no
single fd namespace here): epoll‑encoded fd (`>=0x10000`) → live socket (`sock_poll_bits`
in `kernel/net/socket.c`, no data consumed) → regular vfs file (always ready) → std
streams → invalid. `POLL*` bit values are identical to `EPOLL*`, so the probe feeds both.

**epoll fixed** (`kernel/core/syscall/epoll.c`): the old `epoll_poll_socket` returned
**always‑EPOLLIN** (a lie) and pumped the net stack inline; `sys_epoll_wait` busy‑slept.
Now it scans each watch against the real `fd_poll_state`, supports **level** (re‑report
while ready) vs **edge** (`EPOLLET` → only newly‑set bits), and yields via
`poll_sleep_slice` instead of `timer_sleep`.

**Architectural note (load‑bearing):** `sock_poll()` is the **only** thing that drives
network RX — there is no NIC interrupt or RX thread — so the poll/select/epoll blocking
forms are a *pump → check → yielding‑sleep* loop, not a pure event wait. Removing the pump
needs interrupt‑driven NIC RX (deferred roadmap). This is documented, not a hack.

**Proof:** `sbin/pollselftest` + `bash scripts/pollselect_smoke.sh`:
```
POLLSELECT: PASS poll_ready=1 select_ready=1 poll_timeout=1 mixed_fd=1 epoll_level=1 epoll_edge=1
```

---

## 3. Preemptive scheduler + wait hardening (B7 · PREEMPT‑WAITSAFE‑0)

The preemptive scheduler (`PREEMPT=1`) was proven **fair** (6 never‑yielding `cpuburn`
processes all reach equal beat counts under the timer — cooperative pins five at zero) and
stable (8‑min soak, `panic=0 invariant=0`; B8 signals + B10 poll/select + floattest SSE all
pass under preemption).

**The blocker the soak exposed:** the wait infrastructure relied on **cooperative
scan‑then‑block atomicity** — `sys_waitpid` and `thread_join` say so in comments. Under
preemption a timer IRQ between the scan (event not ready) and the block lets the event fire
its wake *before* the waiter enqueues → lost wakeup → hang forever (reproduced: a child
exited, the parent hung 6+ minutes).

**The fix (PREEMPT‑WAITSAFE‑0):** at the only two indefinite check‑then‑block‑forever
sites — `sys_waitpid` and `thread_join` in `kernel/core/syscall/handlers.c` — under
`#ifdef PREEMPTIVE` replace the indefinite block with a **self‑healing timed block**
(`wait_object_block(wo, timer_get_ticks() + WAIT_RECHECK_MS)`, `WAIT_RECHECK_MS=25`). A
real signal still wakes immediately; a wake lost to the scan/block gap is recovered when
the deadline re‑readies the waiter and its loop re‑scans. The cooperative `#else` keeps the
indefinite block, so the **cooperative kernel is byte‑identical** (`kernel.elf` 513224) —
the hardening is PREEMPT‑only.

**Why this is the complete fix** (verified by grep + read): the only indefinite
check‑then‑block sites in the kernel were `waitpid` and `thread_join`. `futex` already uses
the prepare/commit pattern; `epoll`/`poll`/`select` already use timed blocks; `SYS_CH_WAIT`
and `sys_read_event` are **non‑blocking** readiness polls (userspace yields per frame). So
no other site carries the race.

**Proof:** rebuild `PREEMPT=1` + `STRESS=1`, re‑soak the 6‑burner load — sigtest's
SIGCHLD‑via‑waitpid check that hung forever now prints `SIGTEST RESULT: PASS`, fairness
intact, `panic=0`.

**Status:** the blocker is resolved; a 30‑minute PREEMPT+STRESS soak is the final gate
before flipping `PREEMPT` to the build default (keep a `COOPERATIVE_ONLY` escape hatch).

---

## 4. Open items

- **B7 flip:** make `PREEMPT` the build default once the 30‑min soak + audit green‑light;
  keep `COOPERATIVE_ONLY=1` as the escape hatch.
- **B5 · FB‑WC‑PAT‑0 (T410):** framebuffer write‑combining via page‑level PAT. Design ready
  (program `IA32_PAT` WC at index 1; set PWT on the 2 MB FB PDEs at `~0xFD000000`; gate
  `FB_PAT_WC`). The fps win is only observable on the physical T410 (QEMU's FB is cached),
  so it is deferred to a T410 session for validation.
- **libc signal wiring:** point `signal()/sigaction()/sigprocmask()/sigpending()/raise()`
  at syscalls 107–110, add a `__libc_sigrestorer` trampoline (`mov $109,%rax; syscall`),
  and use the `1<<sig` sigset convention. Land with the shell brick that consumes it.
- **Wait infra (optional upgrade):** the self‑healing timed block is robust; a future pass
  could move `waitpid`/`thread_join` to a true prepare/commit (enqueue‑then‑recheck) for
  zero recovery latency.
- **Deferred roadmap:** writable storage (ext2 write + journal), interrupt‑driven I/O
  (MSI/MSI‑X — also unlocks a clean NIC RX path to retire the `sock_poll` pump), congestion
  control, ASLR/KASLR, GPU modesetting.

---

## Build & prove (quick reference)

```sh
# cooperative default (safe baseline)
bash scripts/quick_build.sh && bash scripts/build_all.sh
bash scripts/sig_smoke.sh           # SIGFULL: PASS ...
bash scripts/pollselect_smoke.sh    # POLLSELECT: PASS ...

# preemptive kernel + stress soak (B7 gate)
PREEMPT=1 bash scripts/quick_build.sh && cp build/kernel-preempt.elf build/kernel.elf
STRESS=1 bash scripts/build_all.sh   # init spawns 6 cpuburn + 3 floattest
# boot headless, grep serial for: SIGTEST RESULT: PASS, fair cpuburn beats, panic=0
```

Gotcha: `build_all.sh` has **two** sbin lists (the canary loop ~line 577 and the copy loop
~line 596); a new `sbin/` app must be added to **both** or it "spawns: not found".
