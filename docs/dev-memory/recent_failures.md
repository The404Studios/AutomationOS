# recent_failures — failure modes + the lesson each one taught

> The most valuable training data: real mistakes and their fixes. Each entry = a trap to avoid.

- **UHCI on the T410 = dead end (EHCI-only PCH).** The USB mouse brick worked perfectly in QEMU
  (PIIX3 has UHCI) but found no controller on the T410 (QM57/Ibex Peak is EHCI-only with rate-
  matching hubs). *Lesson:* QEMU can validate the wrong thing — verify the target chipset, keep an
  honest testability ledger. → spawned `USB-EHCI-0`.
- **ISO contamination read as "USB broke everything".** `grub-mkrescue` over the current `iso/`
  packaged an in-progress desktop regression under the USB kernel; on the T410 it looked like USB
  broke the desktop. *Lesson:* single-variable test image = feature delta vs a known-good baseline
  (FIXED userspace). The "stable" `automationos-t410.iso` had been rebuilt with the bad userspace.
- **"Debug mode" = `SCHED_DEBUG`.** A test ISO looked debug-y because its kernel was a plain
  `quick_build` (SCHED_DEBUG=1 → yellow on-screen markers) and not `T410_SAFE`. *Lesson:* hardware
  test kernels must be `T410_SAFE=1 SCHED_DEBUG=0`; the green corner box was just the battery
  indicator (hidden on QEMU, shown on the laptop).
- **CRLF from a branch checkout breaks bash.** `git checkout` flipped `quick_build.sh` to CRLF on
  Windows → `$'\r': command not found`. *Lesson:* `sed -i 's/\r$//'` shell scripts after a checkout;
  commit with `git -c core.autocrlf=false`. (C files tolerate CRLF; shell scripts don't.)
- **`*/` inside a C comment.** A comment containing `input_report_*/input_sync` closed the comment
  early → "unknown type name" — but only in the gated build that compiled the stray code. *Lesson:*
  never write `*/` in a comment; phrase it `input_report_* and input_sync`.
- **`build_all.sh` ignores `T410_SAFE`** (copies the existing `build/kernel.elf`) → re-stage a
  T410-safe kernel after it, or the ISO ships the wrong kernel.
- **Agent finders ~35–40% false positive even post-verify.** Verify every finding against real
  code/registers before patching; a human/reviewer gate is mandatory.
- **Frozen-tick:** any unbounded wait in a syscall freezes the whole machine (cooperative single
  core). Every device wait needs an iteration cap + a tick-independent delay.
