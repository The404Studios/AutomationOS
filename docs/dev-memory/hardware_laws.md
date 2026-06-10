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
