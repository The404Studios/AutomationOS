# hardware_laws — the non-negotiable rules every brick obeys

These govern every change. They are the reviewer's checklist and the highest-value training
signal: a model that internalizes these is *useful* on this OS; one that doesn't is dangerous.

## The laws

1. **Frozen-tick / bounded waits.** The default scheduler is cooperative single-core; syscalls and
   IRQs run with interrupts disabled and there is no second core to recover a hung wait. **Every
   device/controller/transfer wait is iteration-capped** with a tick-independent delay (an
   `inb(0x80)` spin, not the timer tick). A stuck device must time out, never hang the machine.
2. **Default build unchanged.** New or risky support is **default-OFF behind a build flag**
   (`EHCI_USB`, `USB_UHCI`, `DISK_PERSIST`, `PCH_NIC`, `SMP`, `PREEMPT`, …) or **additive** (new
   syscalls nothing calls yet). A default build must be byte-for-byte the validated configuration.
3. **Gate untestable hardware OFF.** New device init defaults off until validated on the real
   device. A bus stall ≠ a software spin, so iteration caps don't make unproven MMIO safe.
4. **QEMU first, hardware second.** New hardware must pass the no-device and device-present boots
   in QEMU before it is flashed to the T410. (Caveat: QEMU can mislead — its PIIX3 has UHCI the
   T410's EHCI-only PCH lacks. Keep an honest testability ledger.)
5. **No stale-ISO contamination.** A single-variable hardware test image holds the *feature* delta
   only, against a **known-good baseline**. Build test ISOs from FIXED userspace; never repackage a
   contaminated `iso/`. (See [known_good_images.md](known_good_images.md).)
6. **T410-safe profile for hardware images.** `T410_SAFE=1 SCHED_DEBUG=0` (ERMS off, no yellow
   on-screen scheduler markers). A non-T410_SAFE/`SCHED_DEBUG=1` kernel *looks* like "debug mode".
7. **Kernel/userspace separation.** Don't touch userspace/input/compositor semantics in a kernel
   brick (and vice-versa) unless that's the brick.
8. **Small, bisectable commits.** One checkpoint = one commit. Commit only when the user says to.
   Build + boot-smoke must gate every claim.
9. **Verify before claiming.** Check findings against the real code/registers before patching
   (agent finders have a real false-positive rate). "Green" means a build + boot actually passed.
10. **No raw pointers / path truncations.** Untrusted input (paths, parser bytes, lengths) is
    bounds-checked; identity is the full VFS path, never a truncated display label.
11. **No CTRL_RST without SWFLAG (the ME-shared-MDIO law, user-set at NET-P1-0).** On PCH NICs
    (the T410's 82577LM family) the MAC/PHY MDIO link is SHARED with the Management Engine; a
    reset issued without holding the SW/FW semaphore (`EXTCNF_CTRL.SWFLAG`) can hardware-stall
    the bus — unrecoverable in software, immune to iteration caps. If the semaphore cannot be
    acquired, ABORT cleanly and let the (re-runnable, post-desktop) trigger retry later. More
    generally: any device whose bus is shared with firmware gets its risky init DEFERRED out of
    boot behind a runtime trigger, with a serial marker before every risky touch
    (last-line-wins diagnosis).
12. **An idle loop's `hlt` must explicitly enable interrupts: `sti; hlt` (user-set at SMP-F3-5).**
    Never rely on inherited IF. A bare `hlt` inherits whatever IF the last context switch restored;
    if that context ran with interrupts off (idle's restored rflags), the CPU parks PERMANENTLY —
    no timer, no IPI, nothing wakes it. The bug class is invisible until a CPU actually re-enters
    the idle `hlt` path (all of F2 era never did), then flaky-fatal. `sti; hlt` is the safe pair on
    x86: `sti` enables interrupts only after the *next* instruction completes, so no wake can slip
    into the gap before the `hlt`.
13. **IPI vectors must be collision-checked at compile time AND runtime before arming (user-set
    at SMP-G0).** The IDT vector landscape is a shared, silently-overwritable resource: a second
    `idt_set_gate` on a live vector kills the prior owner with no diagnostic (the original
    IPI_RESCHEDULE=0x40 sat dead-on the CPU1 LAPIC timer gate). Every vector block carries
    `_Static_assert`s against the known claimants (exceptions 0x00-0x1F, PIC 0x20-0x2F, per-CPU
    timers, spurious 0xFF) AND a runtime `idt_gate_present()` free-check before claiming —
    occupied ⇒ refuse loudly and stay disarmed, never overwrite.
14. **IPI target APIC ids must come from the proven MADT/AP-start seam, never from
    partially-filled per-CPU structs (user-set at SMP-G0).** A struct that LINKS is not a struct
    that is FILLED: `percpu_data[]` linked (health-monitor definition) but `.apic_id` was never
    written, so the salvage `cpu_to_apic_id(1)` returned 0 — every "CPU1" IPI would have targeted
    the BSP ITSELF. Wrong-target beats link-fail as a failure mode because the strict linker
    can't see it. Resolve CPU→APIC through the seam that demonstrably captured the id
    (`smp_cpu1_apic_id()` from try_start_cpu1's MADT read), with an explicit
    not-yet-captured sentinel that DROPS the send.
15. **Any SMP handler data touched under arbitrary CR3 must pass the shadow-zone/link-map gate
    (user-set at SMP-G0).** Interrupt/IPI handlers run on whatever address space the target CPU
    happens to hold, so their state (stats, queues, locks, flags) must sit below the user link
    base or in the kernel-only direct map — and the placement is PROVEN per build by an nm/link-
    map check in the brick's smoke, not assumed from a linker-script comment (linker.ld's
    "0x200000 user base" was stale; the live base is 0x800000 per userspace.ld). Size such arrays
    for the real machine model, not MAX_CPUS=256.
16. **Never wait for TLB-shootdown ACKs while holding scheduler, runqueue, heap, or filesystem
    locks (user-set at SMP-G2).** An ack wait is a cross-CPU rendezvous: the target must take the
    IPI and run its handler. If the waiter holds a lock the target needs (or the target is
    spinning on a lock the waiter holds), the wait deadlocks both cores — and an IF=0 wait can't
    even take its own interrupts. Shootdown waits are entered ONLY from lock-free context and
    are TSC-bounded (a lost ack times out loudly, never hangs). Enforceable runtime checks:
    `IF==1` asserted at wait entry (catches the cli'd-context class outright — NOTE plain
    spin_lock does NOT cli in this kernel, so this is necessary-not-sufficient), plus the
    bounded timeout itself (a deadlock degrades to a loud timeout, never a hang). Full
    lock-freedom remains a review-checklist discipline at every call site.
17. **BKL-marked syscalls must not block or schedule away while holding the BKL (user-set at
    SMP-H1).** Syscalls run IF=0 on a cooperative kernel: a marked syscall that
    context-switches away leaves the BKL owned by a parked task, and the other CPU's spinners
    wedge forever (the Linux-2.0 release-on-block problem, refused rather than solved in v1).
    Marking discipline: FS / IPC / non-blocking-NET / process-management danger paths only;
    wait/sleep/msgrcv/recv/accept/connect/futex/thread_join/ch_wait stay UNMARKED. Audit every
    new marking for hidden schedule paths (sys_execve aliases spawn = returns; sys_kill defers
    the switch). The bounded-spin watchdog (~2 s, loud one-shot, never unlocked-proceed) turns
    a future violation into a visible failure instead of a silent hang. Release-on-block is
    the sanctioned future path to marking blockers — never "it probably won't contend."
18. **Declared CPU masks are not proof; TLB pinning safety is based on actual per-CR3 run
    history (user-set at SMP-RUNMASK-0).** A multi-CPU `allowed_cpus` is a legal declaration
    (batchdemo); the real TLB hazard is an ADDRESS SPACE that actually EXECUTED on more than
    one CPU. `p->ran_on_cpus` is stamped at the dispatch chokepoints and the pin audit
    aggregates it PER CR3 (threads share an mm; kernel-CR3 residents are exempt — that space
    is G2's shootdown domain). Dying multimask processes are recorded at the exit boundary
    (short-lived tasks are invisible to live walks). The audit is the DESKTOP-SPLIT forcing
    function: the day any user address space runs on two CPUs without per-mm shootdown work,
    the smoke fails loudly. Never weaken it back to declared-mask heuristics.
19. **Default-compiled files with ASSERT/__LINE__ users may not receive gated insertions above
    the last __LINE__ site if byte-identity is required (user-set at SMP-RUNMASK-0).** The
    preprocessor counts lines in FALSE #ifdef branches, so even fully compiled-out code shifts
    the __LINE__ immediates that ASSERT_ALWAYS embeds below it — the default kernel hash
    changes with zero semantic delta. Bisect such breaks at the object level (md5 per .o,
    diff the two builds — minutes, not hours). Insert gated code BELOW a file's last __LINE__
    user, or hook at a call site in a __LINE__-free file instead.
20. **A shared cross-CPU DETECTOR field must update atomically — never a plain read-modify-write
    (user-set at SMP-THREAD-INHERIT-0).** When a field exists to catch a concurrency hazard (e.g.
    `mm_place->ran_on_cpus`, the "one address space on two CPUs" detector), the writers ARE the
    racing CPUs the field detects. A plain `|=` is a read-modify-write that two CPUs can tear,
    losing one CPU's bit — and the lost bit is exactly the evidence of the bug, so the detector
    hides the very thing it exists to catch. Use a locked OR (`__atomic_fetch_or`, SEQ_CST). The
    detector must be no weaker than the hazard. (Per-task fields with a single concurrent writer —
    a task is never on two CPUs at once — do NOT need this; only genuinely shared fields do.)

## Reviewer checklist (a stricter role that can say "no")

For every patch, answer:
- Does the **default build change**? (must be no, unless that's the brick)
- Are **all waits bounded** (iteration cap + tick-independent delay)?
- Is **hardware init gated** off by default?
- Does it **touch userspace** accidentally?
- Does it **preserve the known-good T410 image** (no stale-ISO contamination)?
- Does the **smoke test actually prove the claim** (not just "it built")?
- Any **raw pointers, unbounded loops, or path truncations**?

A patch that fails any of these is rejected, not merged.

## Build/test reflexes

- WSL Arch build: `MSYS_NO_PATHCONV=1 wsl.exe -d Arch bash /mnt/c/.../script.sh` (write script
  files; nested `bash -c "...$var..."` eats loop vars). Branch checkout can flip shell scripts to
  CRLF on Windows — `sed -i 's/\r$//'` before running.
- Commit/build in WSL; **push from Windows PowerShell git** (GCM holds the PAT; verify with
  `git ls-remote`). Commits use `git -c core.autocrlf=false`.
- `build_all.sh` ignores `T410_SAFE`/`SCHED_DEBUG` (it copies the existing `build/kernel.elf`);
  re-stage a T410-safe kernel after it. `quick_build.sh` builds the kernel only.
